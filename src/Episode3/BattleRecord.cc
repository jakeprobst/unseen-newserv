#include "BattleRecord.hh"

#include <phosg/Time.hh>

#include "../CommandFormats.hh"
#include "../SendCommands.hh"

using namespace std;

namespace Episode3 {

BattleRecord::Event::Event(StringReader& r) {
  this->type = r.get<Event::Type>();
  this->timestamp = r.get_u64l();
  switch (this->type) {
    case Event::Type::PLAYER_JOIN:
      this->players.emplace_back(r.get<PlayerEntry>());
      break;
    case Event::Type::PLAYER_LEAVE:
      this->leaving_client_id = r.get_u8();
      break;
    case Event::Type::SET_INITIAL_PLAYERS: {
      uint8_t count = r.get_u8();
      while (this->players.size() < count) {
        this->players.emplace_back(r.get<PlayerEntry>());
      }
      break;
    }
    case Event::Type::CHAT_MESSAGE:
      this->guild_card_number = r.get_u32l();
      [[fallthrough]];
    case Event::Type::GAME_COMMAND:
    case Event::Type::BATTLE_COMMAND:
    case Event::Type::EP3_GAME_COMMAND:
      this->data = r.read(r.get_u16l());
      break;
    default:
      throw logic_error("unknown event type");
  }
}

void BattleRecord::Event::serialize(StringWriter& w) const {
  w.put(this->type);
  w.put_u64l(this->timestamp);
  switch (this->type) {
    case Event::Type::PLAYER_JOIN:
      if (this->players.size() != 1) {
        throw logic_error("player join event does not contain 1 player entry");
      }
      w.put(this->players[0]);
      break;
    case Event::Type::PLAYER_LEAVE:
      w.put_u8(this->leaving_client_id);
      break;
    case Event::Type::SET_INITIAL_PLAYERS:
      w.put_u8(this->players.size());
      for (const auto& player : this->players) {
        w.put(player);
      }
      break;
    case Event::Type::CHAT_MESSAGE:
      w.put_u32l(this->guild_card_number);
      [[fallthrough]];
    case Event::Type::GAME_COMMAND:
    case Event::Type::BATTLE_COMMAND:
    case Event::Type::EP3_GAME_COMMAND:
      w.put_u16l(this->data.size());
      w.write(this->data);
      break;
    default:
      throw logic_error("unknown event type");
  }
}

BattleRecord::BattleRecord(uint32_t behavior_flags)
    : is_writable(true),
      behavior_flags(behavior_flags),
      battle_start_timestamp(0),
      battle_end_timestamp(0) {}

BattleRecord::BattleRecord(const string& data)
    : is_writable(false),
      behavior_flags(0),
      battle_start_timestamp(0),
      battle_end_timestamp(0) {
  StringReader r(data);
  uint64_t signature = r.get_u64l();
  if (signature != this->SIGNATURE) {
    throw runtime_error("incorrect battle record signature");
  }

  this->battle_start_timestamp = r.get_u64l();
  this->battle_end_timestamp = r.get_u64l();
  this->behavior_flags = r.get_u32l();
  while (!r.eof()) {
    this->events.emplace_back(r);
  }
}

string BattleRecord::serialize() const {
  StringWriter w;
  w.put_u64l(this->SIGNATURE);
  w.put_u64l(this->battle_start_timestamp);
  w.put_u64l(this->battle_end_timestamp);
  w.put_u32l(this->behavior_flags);
  for (const auto& ev : this->events) {
    ev.serialize(w);
  }
  return std::move(w.str());
}

bool BattleRecord::writable() const {
  return this->is_writable;
}

bool BattleRecord::battle_in_progress() const {
  return (this->battle_start_timestamp != 0);
}

const BattleRecord::Event* BattleRecord::get_first_event() const {
  if (this->events.empty()) {
    return nullptr;
  }
  return &this->events.front();
}

void BattleRecord::add_player(
    const PlayerLobbyDataDCGC& lobby_data,
    const PlayerInventory& inventory,
    const PlayerDispDataDCPCV3& disp) {
  if (!this->is_writable) {
    throw logic_error("cannot write to battle record");
  }
  if (this->battle_start_timestamp != 0) {
    throw runtime_error("cannot add player during battle");
  }
  Event& ev = this->events.emplace_back();
  ev.type = Event::Type::PLAYER_JOIN;
  ev.timestamp = now();
  auto& player = ev.players.emplace_back();
  player.lobby_data = lobby_data;
  player.inventory = inventory;
  player.disp = disp;
}

void BattleRecord::delete_player(uint8_t client_id) {
  if (!this->is_writable) {
    throw logic_error("cannot write to battle record");
  }
  Event& ev = this->events.emplace_back();
  ev.type = Event::Type::PLAYER_LEAVE;
  ev.timestamp = now();
  ev.leaving_client_id = client_id;
}

void BattleRecord::add_command(Event::Type type, const void* data, size_t size) {
  if (!this->is_writable) {
    throw logic_error("cannot write to battle record");
  }
  Event& ev = this->events.emplace_back();
  ev.type = type;
  ev.timestamp = now();
  ev.data.assign(reinterpret_cast<const char*>(data), size);
}

void BattleRecord::add_command(Event::Type type, string&& data) {
  if (!this->is_writable) {
    throw logic_error("cannot write to battle record");
  }
  Event& ev = this->events.emplace_back();
  ev.type = type;
  ev.timestamp = now();
  ev.data = std::move(data);
}

void BattleRecord::add_chat_message(
    uint32_t guild_card_number, string&& data) {
  if (!this->is_writable) {
    throw logic_error("cannot write to battle record");
  }
  Event& ev = this->events.emplace_back();
  ev.type = Event::Type::CHAT_MESSAGE;
  ev.timestamp = now();
  ev.guild_card_number = guild_card_number;
  ev.data = std::move(data);
}

bool BattleRecord::is_map_definition_event(const Event& ev) {
  if (ev.type == Event::Type::BATTLE_COMMAND) {
    auto& header = check_size_t<G_CardBattleCommandHeader>(ev.data, 0xFFFF);
    if (header.subcommand == 0xB6) {
      auto& header = check_size_t<G_MapSubsubcommand_GC_Ep3_6xB6>(ev.data, 0xFFFF);
      if (header.subsubcommand == 0x41) {
        return true;
      }
    }
  }
  return false;
}

void BattleRecord::set_battle_start_timestamp() {
  if (this->battle_start_timestamp != 0) {
    throw logic_error("battle start timestamp is already set");
  }
  this->battle_start_timestamp = now();

  // First, find the correct map definition subcommand to keep, and execute
  // player join/leave events to get the present players
  size_t num_map_events = 0;
  PlayerEntry players[4];
  bool players_present[4];
  for (auto& ev : this->events) {
    if (ev.type == Event::Type::PLAYER_JOIN) {
      if (ev.players.size() != 1) {
        throw logic_error("player join event does not contain 1 player entry");
      }
      auto& player = ev.players[0];
      if (player.lobby_data.client_id >= 4) {
        throw runtime_error("invalid client ID");
      }
      players[player.lobby_data.client_id] = player;
      players_present[player.lobby_data.client_id] = true;

    } else if (ev.type == Event::Type::PLAYER_LEAVE) {
      if (ev.leaving_client_id >= 4) {
        throw logic_error("invalid client ID");
      }
      players_present[ev.leaving_client_id] = false;

    } else if (ev.type == Event::Type::SET_INITIAL_PLAYERS) {
      throw logic_error("BattleRecord::set_battle_start_timestamp called twice");

    } else if (this->is_map_definition_event(ev)) {
      num_map_events++;
    }
  }

  deque<Event> new_events;

  // Generate the initial players event
  Event initial_ev;
  initial_ev.type = Event::Type::SET_INITIAL_PLAYERS;
  initial_ev.timestamp = this->battle_start_timestamp;
  for (size_t z = 0; z < 4; z++) {
    if (players_present[z]) {
      initial_ev.players.emplace_back(players[z]);
    }
  }
  new_events.emplace_back(std::move(initial_ev));

  // Skip all events before the last map definition event, and only retain
  // battle commands between then and now (since these battle commands will all
  // be replayed at once)
  auto it = this->events.begin();
  for (; it != this->events.end(); it++) {
    if (this->is_map_definition_event(*it)) {
      num_map_events--;
      if (num_map_events == 0) {
        break;
      }
    }
  }
  for (; it != this->events.end(); it++) {
    if (it->type == Event::Type::BATTLE_COMMAND) {
      new_events.emplace_back(std::move(*it));
    }
  }
  this->events = std::move(new_events);
}

void BattleRecord::set_battle_end_timestamp() {
  this->battle_end_timestamp = now();
}

BattleRecordPlayer::BattleRecordPlayer(
    shared_ptr<const BattleRecord> rec,
    shared_ptr<struct event_base> base)
    : record(rec),
      event_it(this->record->events.begin()),
      play_start_timestamp(0),
      base(base),
      next_command_ev(event_new(this->base.get(), -1, EV_TIMEOUT, &BattleRecordPlayer::dispatch_schedule_events, this), event_free) {}

shared_ptr<const BattleRecord> BattleRecordPlayer::get_record() const {
  return this->record;
}

void BattleRecordPlayer::set_lobby(std::shared_ptr<Lobby> l) {
  this->lobby = l;
}

void BattleRecordPlayer::start() {
  if (this->play_start_timestamp == 0) {
    this->play_start_timestamp = now();
    this->schedule_events();
  }
}

void BattleRecordPlayer::dispatch_schedule_events(
    evutil_socket_t, short, void* ctx) {
  reinterpret_cast<BattleRecordPlayer*>(ctx)->schedule_events();
}

void BattleRecordPlayer::schedule_events() {
  // If the lobby is destroyed, we can't replay anything - just return without
  // rescheduling
  auto l = this->lobby.lock();
  if (!l) {
    return;
  }

  for (;;) {
    uint64_t relative_ts = now() - this->play_start_timestamp + this->record->battle_start_timestamp;

    if (this->event_it == this->record->events.end()) {
      if (relative_ts >= this->record->battle_end_timestamp) {
        // If the record is complete and the end timestamp has been reached,
        // send exit commands to all players in the lobby, and don't reschedule
        // the event (it will be deleted along with the Player when the lobby is
        // destroyed, when the last client leaves)
        send_command(l, 0xED, 0x00);

      } else {
        // There are no more events to play, but the battle has not officially
        // ended yet - reschedule the event for the end time
        auto tv = usecs_to_timeval(this->record->battle_end_timestamp - relative_ts);
        event_add(this->next_command_ev.get(), &tv);
      }
      break;

    } else {
      if (this->event_it->timestamp <= relative_ts) {
        // Play the next event
        auto& ev = *this->event_it;
        switch (ev.type) {
          case BattleRecord::Event::Type::PLAYER_JOIN:
            // Technically we can support this, but it should never happen
            throw runtime_error("player join event during battle replay");
          case BattleRecord::Event::Type::PLAYER_LEAVE:
            send_player_leave_notification(l, ev.leaving_client_id);
            break;
          case BattleRecord::Event::Type::SET_INITIAL_PLAYERS:
            // This should have been handled before the lobby was even created
            break;
          case BattleRecord::Event::Type::BATTLE_COMMAND:
            send_command(l, (ev.data.size() >= 0x400) ? 0x6C : 0xC9, 0x00, ev.data);
            break;
          case BattleRecord::Event::Type::GAME_COMMAND:
            send_command(l, (ev.data.size() >= 0x400) ? 0x6C : 0x60, 0x00, ev.data);
            break;
          case BattleRecord::Event::Type::EP3_GAME_COMMAND:
            send_command(l, 0xCB, 0x00, ev.data);
            break;
          case BattleRecord::Event::Type::CHAT_MESSAGE:
            send_chat_message(l, ev.guild_card_number, decode_sjis(ev.data));
            break;
        }
        this->event_it++;

      } else {
        // The next event should not occur yet, so reschedule for the time when
        // it should occur
        auto tv = usecs_to_timeval(this->event_it->timestamp - relative_ts);
        event_add(this->next_command_ev.get(), &tv);
        break;
      }
    }
  }
}

} // namespace Episode3
