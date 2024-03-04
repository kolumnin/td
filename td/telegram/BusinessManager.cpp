//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/BusinessManager.h"

#include "td/telegram/BusinessAwayMessage.h"
#include "td/telegram/BusinessGreetingMessage.h"
#include "td/telegram/BusinessWorkHours.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/DialogLocation.h"
#include "td/telegram/Global.h"
#include "td/telegram/Td.h"
#include "td/telegram/telegram_api.h"

#include "td/utils/buffer.h"
#include "td/utils/Status.h"

namespace td {

class UpdateBusinessLocationQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogLocation location_;

 public:
  explicit UpdateBusinessLocationQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogLocation &&location) {
    location_ = std::move(location);
    int32 flags = 0;
    if (!location_.empty()) {
      flags |= telegram_api::account_updateBusinessLocation::GEO_POINT_MASK;
    }
    if (!location_.get_address().empty()) {
      flags |= telegram_api::account_updateBusinessLocation::ADDRESS_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::account_updateBusinessLocation(flags, location_.get_input_geo_point(), location_.get_address()),
        {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::account_updateBusinessLocation>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    td_->contacts_manager_->on_update_user_location(td_->contacts_manager_->get_my_id(), std::move(location_));

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class UpdateBusinessWorkHoursQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  BusinessWorkHours work_hours_;

 public:
  explicit UpdateBusinessWorkHoursQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(BusinessWorkHours &&work_hours) {
    work_hours_ = std::move(work_hours);
    int32 flags = 0;
    if (!work_hours_.is_empty()) {
      flags |= telegram_api::account_updateBusinessWorkHours::BUSINESS_WORK_HOURS_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::account_updateBusinessWorkHours(flags, work_hours_.get_input_business_work_hours()), {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::account_updateBusinessWorkHours>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    td_->contacts_manager_->on_update_user_work_hours(td_->contacts_manager_->get_my_id(), std::move(work_hours_));

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class UpdateBusinessGreetingMessageQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  BusinessGreetingMessage greeting_message_;

 public:
  explicit UpdateBusinessGreetingMessageQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(BusinessGreetingMessage &&greeting_message) {
    greeting_message_ = std::move(greeting_message);
    int32 flags = 0;
    if (!greeting_message_.is_empty()) {
      flags |= telegram_api::account_updateBusinessGreetingMessage::MESSAGE_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::account_updateBusinessGreetingMessage(
                                                   flags, greeting_message_.get_input_business_greeting_message(td_)),
                                               {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::account_updateBusinessGreetingMessage>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    td_->contacts_manager_->on_update_user_greeting_message(td_->contacts_manager_->get_my_id(),
                                                            std::move(greeting_message_));

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class UpdateBusinessAwayMessageQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  BusinessAwayMessage away_message_;

 public:
  explicit UpdateBusinessAwayMessageQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(BusinessAwayMessage &&away_message) {
    away_message_ = std::move(away_message);
    int32 flags = 0;
    if (!away_message_.is_empty()) {
      flags |= telegram_api::account_updateBusinessAwayMessage::MESSAGE_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::account_updateBusinessAwayMessage(flags, away_message_.get_input_business_away_message(td_)),
        {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::account_updateBusinessAwayMessage>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    td_->contacts_manager_->on_update_user_away_message(td_->contacts_manager_->get_my_id(), std::move(away_message_));

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

BusinessManager::BusinessManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
}

void BusinessManager::tear_down() {
  parent_.reset();
}

void BusinessManager::set_business_location(DialogLocation &&location, Promise<Unit> &&promise) {
  td_->create_handler<UpdateBusinessLocationQuery>(std::move(promise))->send(std::move(location));
}

void BusinessManager::set_business_work_hours(BusinessWorkHours &&work_hours, Promise<Unit> &&promise) {
  td_->create_handler<UpdateBusinessWorkHoursQuery>(std::move(promise))->send(std::move(work_hours));
}

void BusinessManager::set_business_greeting_message(BusinessGreetingMessage &&greeting_message,
                                                    Promise<Unit> &&promise) {
  td_->create_handler<UpdateBusinessGreetingMessageQuery>(std::move(promise))->send(std::move(greeting_message));
}

void BusinessManager::set_business_away_message(BusinessAwayMessage &&away_message, Promise<Unit> &&promise) {
  td_->create_handler<UpdateBusinessAwayMessageQuery>(std::move(promise))->send(std::move(away_message));
}

}  // namespace td
