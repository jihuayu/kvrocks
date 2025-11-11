#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "jsoncons/json.hpp"
#include "storage/storage.h"

namespace redis {

class AclSelector {
 public:
  uint32_t flags;                          // SELECTOR_FLAG_ALLKEYS, ALLCHANNELS, ALLCOMMANDS, etc.
  std::vector<uint64_t> allowed_commands;  // Command permission bitmap, size = USER_COMMAND_BITS_COUNT / 64
  std::vector<uint32_t> allowed_category;  // Command category permission bitmap, size = USER_CATEGORY_BITS_COUNT / 32
  std::vector<std::string> patterns;       // List of key patterns
  std::vector<std::string> channels;       // List of channel patterns
};

class AclUser {
 public:
  bool enabled;                               // Whether the user is enabled
  std::string ns;                             // Namespace of the user
  std::vector<AclSelector> allowed_commands;  // The first is the root selector, the rest are regular selectors
  std::set<std::string> passwords;            // Set of passwords, stored as sha256 hashes. Nopass if set is empty

  jsoncons::json ToJson() const;
  static StatusOr<AclUser> FromJson(const jsoncons::json &json);
};

class AclUserManager {
 public:
  AclUserManager();
  std::shared_ptr<const AclUser> GetUserByIndex(size_t index);
  std::shared_ptr<const AclUser> GetUserByUserName(const std::string &username);
  std::shared_ptr<const AclUser> AuthenticateUser(const std::string &username, const std::string &password);
  std::optional<size_t> GetUserIndex(const std::string &username) const;
  bool UpdateUser(const std::string &username, std::shared_ptr<const AclUser> user);
  bool SetUser(const std::string &username, std::shared_ptr<const AclUser> user);
  bool AddUser(const std::string &username, std::shared_ptr<const AclUser> user);
  bool DeleteUser(const std::string &username);
  void Reset();

 private:
  size_t findFreeSlotLocked() const;
  mutable std::shared_mutex mu_;
  // username to user_array_ index mapping
  std::map<std::string, size_t> username_index_;
  // lock free fixed-size array to store users
  std::array<std::shared_ptr<const AclUser>, 256> user_array_;
};

enum class CommandCategory : uint8_t;

class AclCommandManager {
 public:
  static AclCommandManager &Instance();

  size_t RegisterCommand(const std::string &name, redis::CommandCategory category);
  std::optional<size_t> GetCommandBit(const std::string &name) const;
  StatusOr<std::vector<uint64_t>> BuildBitmapForCommands(const std::vector<std::string> &commands) const;
  std::vector<std::string> CommandsFromBitmap(const std::vector<uint64_t> &bitmap) const;
  std::vector<uint64_t> BuildBitmapForAllCommands() const;
  bool IsCommandAllowed(const std::vector<uint64_t> &bitmap, const std::string &command) const;
  void Seal();

  AclCommandManager(const AclCommandManager &) = delete;
  AclCommandManager &operator=(const AclCommandManager &) = delete;
  ~AclCommandManager() = default;

 private:
  AclCommandManager() = default;

  mutable std::shared_mutex mu_;
  std::map<std::string, size_t> command_bits_;
  size_t next_bit_ = 0;
  std::atomic<bool> sealed_{false};
};

class Acl {
 public:
  explicit Acl(engine::Storage *storage) : storage_(storage) {}

  StatusOr<AclUser> Get(const std::string &username);
  Status Set(const std::string &username, const AclUser &user);
  Status Del(const std::string &username);
  Status LoadAcl();
  Status ApplyReplicatedUpdate(const std::string &username, std::string_view serialized_user);
  Status ApplyReplicatedDeletion(const std::string &username);
  std::optional<size_t> GetUserIndex(const std::string &username);
  std::shared_ptr<const AclUser> GetCachedUserByIndex(size_t index);

 private:
  engine::Storage *storage_;
  std::unique_ptr<AclUserManager> user_manager_;
};

inline constexpr std::string_view kAclStoragePrefix = "acl|";
}  // namespace redis
