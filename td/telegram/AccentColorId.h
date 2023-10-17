//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/ChannelId.h"
#include "td/telegram/ChatId.h"
#include "td/telegram/td_api.h"
#include "td/telegram/UserId.h"

#include "td/utils/common.h"
#include "td/utils/HashTableUtils.h"
#include "td/utils/StringBuilder.h"

#include <type_traits>

namespace td {

class AccentColorId {
  int32 id = -1;

 public:
  AccentColorId() = default;

  explicit constexpr AccentColorId(int32 accent_color_id) : id(accent_color_id) {
  }
  template <class T, typename = std::enable_if_t<std::is_convertible<T, int32>::value>>
  AccentColorId(T accent_color_id) = delete;

  explicit AccentColorId(UserId user_id) : id(user_id.get() % 7) {
  }

  explicit AccentColorId(ChatId chat_id) : id(chat_id.get() % 7) {
  }

  explicit AccentColorId(ChannelId channel_id) : id(channel_id.get() % 7) {
  }

  bool is_valid() const {
    return id >= 0;
  }

  int32 get() const {
    return id;
  }

  bool operator==(const AccentColorId &other) const {
    return id == other.id;
  }

  bool operator!=(const AccentColorId &other) const {
    return id != other.id;
  }

  td_api::object_ptr<td_api::accentColorId> get_accent_color_id_object() const {
    return td_api::make_object<td_api::accentColorId>(id);
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    storer.store_int(id);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    id = parser.fetch_int();
  }
};

struct AccentColorIdHash {
  uint32 operator()(AccentColorId accent_color_id) const {
    return Hash<int32>()(accent_color_id.get());
  }
};

inline StringBuilder &operator<<(StringBuilder &string_builder, AccentColorId accent_color_id) {
  return string_builder << "accent color #" << accent_color_id.get();
}

}  // namespace td
