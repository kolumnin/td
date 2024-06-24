//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/StarManager.h"

#include "td/telegram/AccessRights.h"
#include "td/telegram/AuthManager.h"
#include "td/telegram/ChatManager.h"
#include "td/telegram/DialogId.h"
#include "td/telegram/DialogManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/InputInvoice.h"
#include "td/telegram/MessageExtendedMedia.h"
#include "td/telegram/MessageSender.h"
#include "td/telegram/PasswordManager.h"
#include "td/telegram/Photo.h"
#include "td/telegram/StatisticsManager.h"
#include "td/telegram/Td.h"
#include "td/telegram/telegram_api.h"
#include "td/telegram/UpdatesManager.h"
#include "td/telegram/UserManager.h"

#include "td/utils/algorithm.h"
#include "td/utils/buffer.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/ScopeGuard.h"

namespace td {

class GetStarsTopupOptionsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::starPaymentOptions>> promise_;

 public:
  explicit GetStarsTopupOptionsQuery(Promise<td_api::object_ptr<td_api::starPaymentOptions>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(telegram_api::payments_getStarsTopupOptions()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getStarsTopupOptions>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto results = result_ptr.move_as_ok();
    vector<td_api::object_ptr<td_api::starPaymentOption>> options;
    for (auto &result : results) {
      options.push_back(td_api::make_object<td_api::starPaymentOption>(result->currency_, result->amount_,
                                                                       StarManager::get_star_count(result->stars_),
                                                                       result->store_product_, result->extended_));
    }

    promise_.set_value(td_api::make_object<td_api::starPaymentOptions>(std::move(options)));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetStarsTransactionsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::starTransactions>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetStarsTransactionsQuery(Promise<td_api::object_ptr<td_api::starTransactions>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &offset, int32 limit,
            td_api::object_ptr<td_api::StarTransactionDirection> &&direction) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Have no access to the chat"));
    }
    int32 flags = 0;
    if (direction != nullptr) {
      switch (direction->get_id()) {
        case td_api::starTransactionDirectionIncoming::ID:
          flags |= telegram_api::payments_getStarsTransactions::INBOUND_MASK;
          break;
        case td_api::starTransactionDirectionOutgoing::ID:
          flags |= telegram_api::payments_getStarsTransactions::OUTBOUND_MASK;
          break;
        default:
          UNREACHABLE();
      }
    }
    if (td_->auth_manager_->is_bot()) {
      flags |= telegram_api::payments_getStarsTransactions::ASCENDING_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::payments_getStarsTransactions(
        flags, false /*ignored*/, false /*ignored*/, false /*ignored*/, std::move(input_peer), offset, limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getStarsTransactions>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    td_->user_manager_->on_get_users(std::move(result->users_), "GetStarsTransactionsQuery");
    td_->chat_manager_->on_get_chats(std::move(result->chats_), "GetStarsTransactionsQuery");

    vector<td_api::object_ptr<td_api::starTransaction>> transactions;
    for (auto &transaction : result->history_) {
      td_api::object_ptr<td_api::productInfo> product_info;
      string bot_payload;
      if (!transaction->title_.empty() || !transaction->description_.empty() || transaction->photo_ != nullptr) {
        auto photo = get_web_document_photo(td_->file_manager_.get(), std::move(transaction->photo_), DialogId());
        product_info = get_product_info_object(td_, transaction->title_, transaction->description_, photo);
      }
      if (!transaction->bot_payload_.empty()) {
        if (td_->auth_manager_->is_bot()) {
          bot_payload = transaction->bot_payload_.as_slice().str();
        } else if (dialog_id_.get_type() != DialogType::User ||
                   !td_->user_manager_->is_user_bot(dialog_id_.get_user_id())) {
          LOG(ERROR) << "Receive Star transaction with bot payload";
        }
      }
      auto partner = [&]() -> td_api::object_ptr<td_api::StarTransactionPartner> {
        switch (transaction->peer_->get_id()) {
          case telegram_api::starsTransactionPeerUnsupported::ID:
            return td_api::make_object<td_api::starTransactionPartnerUnsupported>();
          case telegram_api::starsTransactionPeerPremiumBot::ID:
            return td_api::make_object<td_api::starTransactionPartnerTelegram>();
          case telegram_api::starsTransactionPeerAppStore::ID:
            return td_api::make_object<td_api::starTransactionPartnerAppStore>();
          case telegram_api::starsTransactionPeerPlayMarket::ID:
            return td_api::make_object<td_api::starTransactionPartnerGooglePlay>();
          case telegram_api::starsTransactionPeerFragment::ID: {
            auto state = [&]() -> td_api::object_ptr<td_api::RevenueWithdrawalState> {
              if (transaction->transaction_date_ > 0) {
                SCOPE_EXIT {
                  transaction->transaction_date_ = 0;
                  transaction->transaction_url_.clear();
                };
                return td_api::make_object<td_api::revenueWithdrawalStateSucceeded>(transaction->transaction_date_,
                                                                                    transaction->transaction_url_);
              }
              if (transaction->pending_) {
                SCOPE_EXIT {
                  transaction->pending_ = false;
                };
                return td_api::make_object<td_api::revenueWithdrawalStatePending>();
              }
              if (transaction->failed_) {
                SCOPE_EXIT {
                  transaction->failed_ = false;
                };
                return td_api::make_object<td_api::revenueWithdrawalStateFailed>();
              }
              if (!transaction->refund_) {
                LOG(ERROR) << "Receive " << to_string(transaction);
              }
              return nullptr;
            }();
            return td_api::make_object<td_api::starTransactionPartnerFragment>(std::move(state));
          }
          case telegram_api::starsTransactionPeer::ID: {
            DialogId dialog_id(
                static_cast<const telegram_api::starsTransactionPeer *>(transaction->peer_.get())->peer_);
            if (dialog_id.get_type() == DialogType::User) {
              auto user_id = dialog_id.get_user_id();
              if (td_->auth_manager_->is_bot() == td_->user_manager_->is_user_bot(user_id)) {
                LOG(ERROR) << "Receive star transaction with " << user_id;
                return td_api::make_object<td_api::starTransactionPartnerUnsupported>();
              }
              SCOPE_EXIT {
                bot_payload.clear();
              };
              return td_api::make_object<td_api::starTransactionPartnerBot>(
                  td_->user_manager_->get_user_id_object(user_id, "starTransactionPartnerBot"), std::move(product_info),
                  bot_payload);
            }
            if (td_->dialog_manager_->is_broadcast_channel(dialog_id)) {
              SCOPE_EXIT {
                transaction->msg_id_ = 0;
                transaction->extended_media_.clear();
              };
              auto message_id = MessageId(ServerMessageId(transaction->msg_id_));
              if (message_id != MessageId() && !message_id.is_valid()) {
                LOG(ERROR) << "Receive " << message_id << " in " << to_string(transaction);
                message_id = MessageId();
              }
              auto extended_media =
                  transform(std::move(transaction->extended_media_), [td = td_, dialog_id](auto &&media) {
                    return MessageExtendedMedia(td, std::move(media), dialog_id).get_message_extended_media_object(td);
                  });
              return td_api::make_object<td_api::starTransactionPartnerChannel>(
                  td_->dialog_manager_->get_chat_id_object(dialog_id, "starTransactionPartnerChannel"),
                  message_id.get(), std::move(extended_media));
            }
            LOG(ERROR) << "Receive star transaction with " << dialog_id;
            return td_api::make_object<td_api::starTransactionPartnerUnsupported>();
          }
          case telegram_api::starsTransactionPeerAds::ID:
            return td_api::make_object<td_api::starTransactionPartnerTelegramAds>();
          default:
            UNREACHABLE();
        }
      }();
      auto star_transaction = td_api::make_object<td_api::starTransaction>(
          transaction->id_, StarManager::get_star_count(transaction->stars_, true), transaction->refund_,
          transaction->date_, std::move(partner));
      if (star_transaction->partner_->get_id() != td_api::starTransactionPartnerUnsupported::ID) {
        if (product_info != nullptr) {
          LOG(ERROR) << "Receive product info with " << to_string(star_transaction);
        }
        if (!bot_payload.empty()) {
          LOG(ERROR) << "Receive bot payload with " << to_string(star_transaction);
        }
        if (transaction->transaction_date_ || !transaction->transaction_url_.empty() || transaction->pending_ ||
            transaction->failed_) {
          LOG(ERROR) << "Receive withdrawal state with " << to_string(star_transaction);
        }
        if (transaction->msg_id_ != 0) {
          LOG(ERROR) << "Receive message identifier with " << to_string(star_transaction);
        }
      }
      transactions.push_back(std::move(star_transaction));
    }

    promise_.set_value(td_api::make_object<td_api::starTransactions>(
        StarManager::get_star_count(result->balance_, true), std::move(transactions), result->next_offset_));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "GetStarsTransactionsQuery");
    promise_.set_error(std::move(status));
  }
};

class RefundStarsChargeQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit RefundStarsChargeQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(telegram_api::object_ptr<telegram_api::InputUser> &&input_user, const string &telegram_payment_charge_id) {
    send_query(G()->net_query_creator().create(
        telegram_api::payments_refundStarsCharge(std::move(input_user), telegram_payment_charge_id)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_refundStarsCharge>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for RefundStarsChargeQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

static td_api::object_ptr<td_api::starRevenueStatus> convert_stars_revenue_status(
    telegram_api::object_ptr<telegram_api::starsRevenueStatus> obj) {
  CHECK(obj != nullptr);
  int32 next_withdrawal_in = 0;
  if (obj->withdrawal_enabled_ && obj->next_withdrawal_at_ > 0) {
    next_withdrawal_in = max(obj->next_withdrawal_at_ - G()->unix_time(), 1);
  }
  return td_api::make_object<td_api::starRevenueStatus>(
      StarManager::get_star_count(obj->overall_revenue_), StarManager::get_star_count(obj->current_balance_),
      StarManager::get_star_count(obj->available_balance_), obj->withdrawal_enabled_, next_withdrawal_in);
}

class GetStarsRevenueStatsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::starRevenueStatistics>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetStarsRevenueStatsQuery(Promise<td_api::object_ptr<td_api::starRevenueStatistics>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, bool is_dark) {
    dialog_id_ = dialog_id;

    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Have no access to the chat"));
    }

    int32 flags = 0;
    if (is_dark) {
      flags |= telegram_api::payments_getStarsRevenueStats::DARK_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::payments_getStarsRevenueStats(flags, false /*ignored*/, std::move(input_peer))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getStarsRevenueStats>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetStarsRevenueStatsQuery: " << to_string(ptr);
    promise_.set_value(td_api::make_object<td_api::starRevenueStatistics>(
        StatisticsManager::convert_stats_graph(std::move(ptr->revenue_graph_)),
        convert_stars_revenue_status(std::move(ptr->status_)),
        ptr->usd_rate_ > 0 ? clamp(ptr->usd_rate_ * 1e2, 1e-18, 1e18) : 1.3));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "GetStarsRevenueStatsQuery");
    promise_.set_error(std::move(status));
  }
};

class GetStarsRevenueWithdrawalUrlQuery final : public Td::ResultHandler {
  Promise<string> promise_;
  DialogId dialog_id_;

 public:
  explicit GetStarsRevenueWithdrawalUrlQuery(Promise<string> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, int64 star_count,
            telegram_api::object_ptr<telegram_api::InputCheckPasswordSRP> input_check_password) {
    dialog_id_ = dialog_id;

    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Have no access to the chat"));
    }

    send_query(G()->net_query_creator().create(telegram_api::payments_getStarsRevenueWithdrawalUrl(
        std::move(input_peer), star_count, std::move(input_check_password))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getStarsRevenueWithdrawalUrl>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(std::move(result_ptr.ok_ref()->url_));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "GetStarsRevenueWithdrawalUrlQuery");
    promise_.set_error(std::move(status));
  }
};

class GetStarsRevenueAdsAccountUrlQuery final : public Td::ResultHandler {
  Promise<string> promise_;
  DialogId dialog_id_;

 public:
  explicit GetStarsRevenueAdsAccountUrlQuery(Promise<string> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id) {
    dialog_id_ = dialog_id;

    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Have no access to the chat"));
    }

    send_query(
        G()->net_query_creator().create(telegram_api::payments_getStarsRevenueAdsAccountUrl(std::move(input_peer))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getStarsRevenueAdsAccountUrl>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(std::move(result_ptr.ok_ref()->url_));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "GetStarsRevenueAdsAccountUrlQuery");
    promise_.set_error(std::move(status));
  }
};

StarManager::StarManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
}

void StarManager::tear_down() {
  parent_.reset();
}

Status StarManager::can_manage_stars(DialogId dialog_id, bool allow_self) const {
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      auto user_id = dialog_id.get_user_id();
      if (allow_self && user_id == td_->user_manager_->get_my_id()) {
        break;
      }
      TRY_RESULT(bot_data, td_->user_manager_->get_bot_data(user_id));
      if (!bot_data.can_be_edited) {
        return Status::Error(400, "The bot isn't owned");
      }
      break;
    }
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      if (!td_->chat_manager_->is_broadcast_channel(channel_id)) {
        return Status::Error(400, "Chat is not a channel");
      }
      if (!td_->chat_manager_->get_channel_permissions(channel_id).is_creator() && !allow_self) {
        return Status::Error(400, "Not enough rights");
      }
      break;
    }
    default:
      return Status::Error(400, "Unallowed chat specified");
  }
  return Status::OK();
}

void StarManager::get_star_payment_options(Promise<td_api::object_ptr<td_api::starPaymentOptions>> &&promise) {
  td_->create_handler<GetStarsTopupOptionsQuery>(std::move(promise))->send();
}

void StarManager::get_star_transactions(td_api::object_ptr<td_api::MessageSender> owner_id, const string &offset,
                                        int32 limit, td_api::object_ptr<td_api::StarTransactionDirection> &&direction,
                                        Promise<td_api::object_ptr<td_api::starTransactions>> &&promise) {
  TRY_RESULT_PROMISE(promise, dialog_id, get_message_sender_dialog_id(td_, owner_id, true, false));
  TRY_STATUS_PROMISE(promise, can_manage_stars(dialog_id, true));
  if (limit < 0) {
    return promise.set_error(Status::Error(400, "Limit must be non-negative"));
  }
  td_->create_handler<GetStarsTransactionsQuery>(std::move(promise))
      ->send(dialog_id, offset, limit, std::move(direction));
}

void StarManager::refund_star_payment(UserId user_id, const string &telegram_payment_charge_id,
                                      Promise<Unit> &&promise) {
  TRY_RESULT_PROMISE(promise, input_user, td_->user_manager_->get_input_user(user_id));
  td_->create_handler<RefundStarsChargeQuery>(std::move(promise))
      ->send(std::move(input_user), telegram_payment_charge_id);
}

void StarManager::get_star_revenue_statistics(const td_api::object_ptr<td_api::MessageSender> &owner_id, bool is_dark,
                                              Promise<td_api::object_ptr<td_api::starRevenueStatistics>> &&promise) {
  TRY_RESULT_PROMISE(promise, dialog_id, get_message_sender_dialog_id(td_, owner_id, true, false));
  TRY_STATUS_PROMISE(promise, can_manage_stars(dialog_id));
  td_->create_handler<GetStarsRevenueStatsQuery>(std::move(promise))->send(dialog_id, is_dark);
}

void StarManager::get_star_withdrawal_url(const td_api::object_ptr<td_api::MessageSender> &owner_id, int64 star_count,
                                          const string &password, Promise<string> &&promise) {
  TRY_RESULT_PROMISE(promise, dialog_id, get_message_sender_dialog_id(td_, owner_id, true, false));
  TRY_STATUS_PROMISE(promise, can_manage_stars(dialog_id));
  if (password.empty()) {
    return promise.set_error(Status::Error(400, "PASSWORD_HASH_INVALID"));
  }
  send_closure(
      td_->password_manager_, &PasswordManager::get_input_check_password_srp, password,
      PromiseCreator::lambda([actor_id = actor_id(this), dialog_id, star_count, promise = std::move(promise)](
                                 Result<telegram_api::object_ptr<telegram_api::InputCheckPasswordSRP>> result) mutable {
        if (result.is_error()) {
          return promise.set_error(result.move_as_error());
        }
        send_closure(actor_id, &StarManager::send_get_star_withdrawal_url_query, dialog_id, star_count,
                     result.move_as_ok(), std::move(promise));
      }));
}

void StarManager::send_get_star_withdrawal_url_query(
    DialogId dialog_id, int64 star_count,
    telegram_api::object_ptr<telegram_api::InputCheckPasswordSRP> input_check_password, Promise<string> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  td_->create_handler<GetStarsRevenueWithdrawalUrlQuery>(std::move(promise))
      ->send(dialog_id, star_count, std::move(input_check_password));
}

void StarManager::get_star_ad_account_url(const td_api::object_ptr<td_api::MessageSender> &owner_id,
                                          Promise<string> &&promise) {
  TRY_RESULT_PROMISE(promise, dialog_id, get_message_sender_dialog_id(td_, owner_id, true, false));
  TRY_STATUS_PROMISE(promise, can_manage_stars(dialog_id));
  td_->create_handler<GetStarsRevenueAdsAccountUrlQuery>(std::move(promise))->send(dialog_id);
}

void StarManager::on_update_stars_revenue_status(
    telegram_api::object_ptr<telegram_api::updateStarsRevenueStatus> &&update) {
  DialogId dialog_id(update->peer_);
  if (can_manage_stars(dialog_id).is_error()) {
    LOG(ERROR) << "Receive " << to_string(update);
    return;
  }
  send_closure(G()->td(), &Td::send_update,
               td_api::make_object<td_api::updateStarRevenueStatus>(
                   get_message_sender_object(td_, dialog_id, "updateStarRevenueStatus"),
                   convert_stars_revenue_status(std::move(update->status_))));
}

int64 StarManager::get_star_count(int64 amount, bool allow_negative) {
  auto max_amount = static_cast<int64>(1) << 51;
  if (amount < 0) {
    if (!allow_negative) {
      LOG(ERROR) << "Receive star amount = " << amount;
      return 0;
    }
    if (amount < -max_amount) {
      LOG(ERROR) << "Receive star amount = " << amount;
      return -max_amount;
    }
  }
  if (amount > max_amount) {
    LOG(ERROR) << "Receive star amount = " << amount;
    return max_amount;
  }
  return amount;
}

}  // namespace td
