//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/BusinessRecipients.h"

#include "td/utils/common.h"
#include "td/utils/tl_helpers.h"

namespace td {

template <class StorerT>
void BusinessRecipients::store(StorerT &storer) const {
  bool has_user_ids = !user_ids_.empty();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(existing_chats_);
  STORE_FLAG(new_chats_);
  STORE_FLAG(contacts_);
  STORE_FLAG(non_contacts_);
  STORE_FLAG(exclude_selected_);
  END_STORE_FLAGS();
  if (has_user_ids) {
    td::store(user_ids_, storer);
  }
}

template <class ParserT>
void BusinessRecipients::parse(ParserT &parser) {
  bool has_user_ids = !user_ids_.empty();
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(existing_chats_);
  PARSE_FLAG(new_chats_);
  PARSE_FLAG(contacts_);
  PARSE_FLAG(non_contacts_);
  PARSE_FLAG(exclude_selected_);
  END_PARSE_FLAGS();
  if (has_user_ids) {
    td::parse(user_ids_, parser);
  }
}

}  // namespace td
