#include "ReplaySession.hh"

#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <phosg/Time.hh>

#include "Loggers.hh"
#include "Server.hh"
#include "Shell.hh"

using namespace std;

ReplaySession::Event::Event(Type type, uint64_t client_id, size_t line_num)
    : type(type),
      client_id(client_id),
      allow_size_disparity(false),
      complete(false),
      line_num(line_num) {}

string ReplaySession::Event::str() const {
  string ret;
  if (this->type == Type::CONNECT) {
    ret = string_printf("Event[%" PRIu64 ", CONNECT", this->client_id);
  } else if (this->type == Type::DISCONNECT) {
    ret = string_printf("Event[%" PRIu64 ", DISCONNECT", this->client_id);
  } else if (this->type == Type::SEND) {
    ret = string_printf("Event[%" PRIu64 ", SEND %04zX", this->client_id, this->data.size());
  } else if (this->type == Type::RECEIVE) {
    ret = string_printf("Event[%" PRIu64 ", RECEIVE %04zX", this->client_id, this->data.size());
  }
  if (this->allow_size_disparity) {
    ret += ", size disparity allowed";
  }
  if (this->complete) {
    ret += ", done";
  }
  ret += string_printf(", ev-line %zu]", this->line_num);
  return ret;
}

ReplaySession::Client::Client(
    ReplaySession* session, uint64_t id, uint16_t port, GameVersion version)
    : id(id),
      port(port),
      version(version),
      channel(
          this->version,
          &ReplaySession::dispatch_on_command_received,
          &ReplaySession::dispatch_on_error,
          session,
          string_printf("R-%" PRIX64, this->id)) {}

string ReplaySession::Client::str() const {
  return string_printf("Client[%" PRIu64 ", T-%hu, %s]",
      this->id, this->port, name_for_version(this->version));
}

shared_ptr<ReplaySession::Event> ReplaySession::create_event(
    Event::Type type, shared_ptr<Client> c, size_t line_num) {
  shared_ptr<Event> event(new Event(type, c->id, line_num));
  if (!this->last_event.get()) {
    this->first_event = event;
  } else {
    this->last_event->next_event = event;
  }
  this->last_event = event;
  if (type == Event::Type::RECEIVE) {
    c->receive_events.emplace_back(event);
  }
  return event;
}

void ReplaySession::check_for_password(shared_ptr<const Event> ev) const {
  auto version = this->clients.at(ev->client_id)->version;

  auto check_pw = [&](const string& pw) {
    if (!this->required_password.empty() && !pw.empty() && (pw != this->required_password)) {
      print_data(stderr, ev->data, 0, nullptr, PrintDataFlags::PRINT_ASCII | PrintDataFlags::OFFSET_16_BITS);
      throw runtime_error(string_printf("(ev-line %zu) sent password is incorrect", ev->line_num));
    }
  };
  auto check_ak = [&](const string& ak) {
    if (this->required_access_key.empty() || ak.empty()) {
      return;
    }
    string ref_access_key;
    if (version == GameVersion::DC || version == GameVersion::PC || version == GameVersion::PATCH) {
      ref_access_key = this->required_access_key.substr(0, 8);
    } else {
      ref_access_key = this->required_access_key;
    }
    if (ak != ref_access_key) {
      print_data(stderr, ev->data, 0, nullptr, PrintDataFlags::PRINT_ASCII | PrintDataFlags::OFFSET_16_BITS);
      throw runtime_error(string_printf("(ev-line %zu) sent access key is incorrect", ev->line_num));
    }
  };
  auto check_either = [&](const string& s) {
    try {
      check_ak(s);
    } catch (const exception&) {
      check_pw(s);
    }
  };

  const void* cmd_data = ev->data.data() + ((version == GameVersion::BB) ? 8 : 4);
  size_t cmd_size = ev->data.size() - ((version == GameVersion::BB) ? 8 : 4);

  switch (version) {
    case GameVersion::PATCH: {
      const auto& header = check_size_t<PSOCommandHeaderPC>(ev->data, 0xFFFF);
      if (header.command == 0x04) {
        check_either(check_size_t<C_Login_Patch_04>(cmd_data, cmd_size).password);
      }
      break;
    }
    case GameVersion::PC: {
      const auto& header = check_size_t<PSOCommandHeaderPC>(ev->data, 0xFFFF);
      if (header.command == 0x03) {
        check_ak(check_size_t<C_LegacyLogin_PC_V3_03>(cmd_data, cmd_size).access_key2);
      } else if (header.command == 0x04) {
        check_ak(check_size_t<C_LegacyLogin_PC_V3_04>(cmd_data, cmd_size).access_key);
      } else if (header.command == 0x9A) {
        const auto& cmd = check_size_t<C_Login_DC_PC_V3_9A>(cmd_data, cmd_size);
        check_ak(cmd.v1_access_key);
        check_ak(cmd.access_key);
        check_ak(cmd.access_key2);
      } else if (header.command == 0x9C) {
        const auto& cmd = check_size_t<C_Register_DC_PC_V3_9C>(cmd_data, cmd_size);
        check_ak(cmd.access_key);
        check_pw(cmd.password);
      } else if (header.command == 0x9D) {
        const auto& cmd = check_size_t<C_Login_DC_PC_GC_9D>(
            cmd_data, cmd_size, sizeof(C_LoginExtended_PC_9D));
        check_ak(cmd.v1_access_key);
        check_ak(cmd.access_key);
        check_ak(cmd.access_key2);
      }
      break;
    }
    case GameVersion::DC:
    case GameVersion::GC:
    case GameVersion::XB: {
      const auto& header = check_size_t<PSOCommandHeaderDCV3>(ev->data, 0xFFFF);
      if (header.command == 0x03) {
        check_ak(check_size_t<C_LegacyLogin_PC_V3_03>(cmd_data, cmd_size).access_key2);
      } else if (header.command == 0x04) {
        check_ak(check_size_t<C_LegacyLogin_PC_V3_04>(cmd_data, cmd_size).access_key);
      } else if (header.command == 0x90) {
        check_ak(check_size_t<C_LoginV1_DC_PC_V3_90>(cmd_data, cmd_size, 0xFFFF).access_key);
      } else if (header.command == 0x93) {
        const auto& cmd = check_size_t<C_LoginV1_DC_93>(
            cmd_data, cmd_size, sizeof(C_LoginExtendedV1_DC_93));
        check_ak(cmd.access_key);
      } else if (header.command == 0x9A) {
        const auto& cmd = check_size_t<C_Login_DC_PC_V3_9A>(cmd_data, cmd_size);
        check_ak(cmd.v1_access_key);
        check_ak(cmd.access_key);
        check_ak(cmd.access_key2);
      } else if (header.command == 0x9C) {
        const auto& cmd = check_size_t<C_Register_DC_PC_V3_9C>(cmd_data, cmd_size);
        check_ak(cmd.access_key);
        check_pw(cmd.password);
      } else if (header.command == 0x9D) {
        const auto& cmd = check_size_t<C_Login_DC_PC_GC_9D>(
            cmd_data, cmd_size, sizeof(C_LoginExtended_DC_GC_9D));
        check_ak(cmd.v1_access_key);
        check_ak(cmd.access_key);
        check_ak(cmd.access_key2);
      } else if (header.command == 0x9E) {
        if (version == GameVersion::GC) {
          const auto& cmd = check_size_t<C_Login_GC_9E>(
              cmd_data, cmd_size, sizeof(C_LoginExtended_GC_9E));
          check_ak(cmd.access_key);
          check_ak(cmd.access_key2);
        } else { // XB
          const auto& cmd = check_size_t<C_Login_XB_9E>(
              cmd_data, cmd_size, sizeof(C_LoginExtended_XB_9E));
          check_ak(cmd.access_key);
          check_ak(cmd.access_key2);
        }
      } else if (header.command == 0xDB) {
        const auto& cmd = check_size_t<C_VerifyLicense_V3_DB>(cmd_data, cmd_size);
        check_ak(cmd.access_key);
        check_ak(cmd.access_key2);
        check_pw(cmd.password);
      }
      break;
    }
    case GameVersion::BB: {
      const auto& header = check_size_t<PSOCommandHeaderBB>(ev->data, 0xFFFF);
      if (header.command == 0x04) {
        check_pw(check_size_t<C_LegacyLogin_BB_04>(cmd_data, cmd_size).password);
      } else if (header.command == 0x93) {
        check_pw(check_size_t<C_Login_BB_93>(cmd_data, cmd_size).password);
      } else if (header.command == 0x9C) {
        check_pw(check_size_t<C_Register_BB_9C>(cmd_data, cmd_size).password);
      } else if (header.command == 0x9E) {
        check_pw(check_size_t<C_LoginExtended_BB_9E>(cmd_data, cmd_size).password);
      } else if (header.command == 0xDB) {
        check_pw(check_size_t<C_VerifyLicense_BB_DB>(cmd_data, cmd_size).password);
      }
      break;
    }
    default:
      throw logic_error("invalid game version");
  }
}

void ReplaySession::apply_default_mask(shared_ptr<Event> ev) {
  auto version = this->clients.at(ev->client_id)->version;

  void* cmd_data = ev->data.data() + ((version == GameVersion::BB) ? 8 : 4);
  size_t cmd_size = ev->data.size() - ((version == GameVersion::BB) ? 8 : 4);
  void* mask_data = ev->mask.data() + ((version == GameVersion::BB) ? 8 : 4);
  size_t mask_size = ev->mask.size() - ((version == GameVersion::BB) ? 8 : 4);

  switch (version) {
    case GameVersion::PATCH: {
      const auto& header = check_size_t<PSOCommandHeaderPC>(ev->data, 0xFFFF);
      if (header.command == 0x02) {
        auto& cmd_mask = check_size_t<S_ServerInit_Patch_02>(mask_data, mask_size);
        cmd_mask.server_key = 0;
        cmd_mask.client_key = 0;
      }
      break;
    }
    case GameVersion::DC:
    case GameVersion::PC:
    case GameVersion::GC:
    case GameVersion::XB: {
      uint8_t command;
      if (version == GameVersion::PC) {
        command = check_size_t<PSOCommandHeaderPC>(ev->data, 0xFFFF).command;
      } else { // V3
        command = check_size_t<PSOCommandHeaderDCV3>(ev->data, 0xFFFF).command;
      }
      switch (command) {
        case 0x02:
        case 0x17:
        case 0x91:
        case 0x9B: {
          auto& mask = check_size_t<S_ServerInitDefault_DC_PC_V3_02_17_91_9B>(
              mask_data, mask_size, 0xFFFF);
          mask.server_key = 0;
          mask.client_key = 0;
          break;
        }
        case 0x19: {
          if (mask_size == sizeof(S_ReconnectSplit_19)) {
            auto& mask = check_size_t<S_ReconnectSplit_19>(mask_data, mask_size);
            mask.pc_address = 0;
            mask.gc_address = 0;
          } else {
            auto& mask = check_size_t<S_Reconnect_19>(mask_data, mask_size);
            mask.address = 0;
          }
          break;
        }
        case 0x41: {
          if (version == GameVersion::PC) {
            auto& mask = check_size_t<S_GuildCardSearchResult_PC_41>(mask_data, mask_size);
            mask.reconnect_command.address = 0;
          } else if (version == GameVersion::BB) {
            auto& mask = check_size_t<S_GuildCardSearchResult_BB_41>(mask_data, mask_size);
            mask.reconnect_command.address = 0;
          } else { // V3
            auto& mask = check_size_t<S_GuildCardSearchResult_DC_V3_41>(mask_data, mask_size);
            mask.reconnect_command.address = 0;
          }
          break;
        }
        case 0x64: {
          if (version == GameVersion::PC) {
            auto& mask = check_size_t<S_JoinGame_PC_64>(mask_data, mask_size);
            mask.variations.clear(0);
            mask.rare_seed = 0;
          } else { // V3
            auto& mask = check_size_t<S_JoinGame_DC_GC_64>(
                mask_data, mask_size, sizeof(S_JoinGame_GC_Ep3_64));
            mask.variations.clear(0);
            mask.rare_seed = 0;
          }
          break;
        }
        case 0xB1: {
          for (size_t x = 4; x < ev->mask.size(); x++) {
            ev->mask[x] = 0;
          }
          break;
        }
        case 0xC9: {
          if (mask_size == 0xCC) {
            auto& mask = check_size_t<G_ServerVersionStrings_GC_Ep3_6xB4x46>(
                mask_data, mask_size);
            mask.version_signature.clear(0);
            mask.date_str1.clear(0);
            mask.date_str2.clear(0);
          }
          break;
        }
        case 0x6C: {
          if (version == GameVersion::GC && mask_size >= 0x14) {
            const auto& cmd = check_size_t<G_MapList_GC_Ep3_6xB6x40>(cmd_data, cmd_size, 0xFFFF);
            if ((cmd.header.header.basic_header.subcommand == 0xB6) &&
                (cmd.header.subsubcommand == 0x40)) {
              check_size_t<PSOCommandHeaderDCV3>(ev->mask, 0xFFFF).size = 0;
              auto& mask = check_size_t<G_MapList_GC_Ep3_6xB6x40>(mask_data, mask_size, 0xFFFF);
              mask.header.header.size = 0;
              mask.compressed_data_size = 0;
              ev->allow_size_disparity = true;
              for (size_t z = sizeof(PSOCommandHeaderDCV3) + sizeof(G_MapList_GC_Ep3_6xB6x40); z < ev->mask.size(); z++) {
                ev->mask[z] = 0;
              }
            }
          }
          break;
        }
      }
      break;
    }
    case GameVersion::BB: {
      uint16_t command = check_size_t<PSOCommandHeaderBB>(ev->data, 0xFFFF).command;
      switch (command) {
        case 0x0003: {
          auto& mask = check_size_t<S_ServerInitDefault_BB_03_9B>(mask_data, mask_size, 0xFFFF);
          mask.server_key.clear(0);
          mask.client_key.clear(0);
          break;
        }
        case 0x0019: {
          auto& mask = check_size_t<S_Reconnect_19>(mask_data, mask_size);
          mask.address = 0;
          break;
        }
        case 0x0064: {
          auto& mask = check_size_t<S_JoinGame_BB_64>(mask_data, mask_size);
          mask.variations.clear(0);
          mask.rare_seed = 0;
          break;
        }
        case 0x00B1: {
          for (size_t x = 8; x < ev->mask.size(); x++) {
            ev->mask[x] = 0;
          }
          break;
        }
        case 0x00E6: {
          auto& mask = check_size_t<S_ClientInit_BB_00E6>(mask_data, mask_size);
          mask.team_id = 0;
          break;
        }
      }
      break;
    }
    default:
      throw logic_error("invalid game version");
  }
}

ReplaySession::ReplaySession(
    shared_ptr<struct event_base> base,
    FILE* input_log,
    shared_ptr<ServerState> state,
    const string& required_access_key,
    const string& required_password)
    : state(state),
      required_access_key(required_access_key),
      required_password(required_password),
      base(base),
      commands_sent(0),
      bytes_sent(0),
      commands_received(0),
      bytes_received(0) {
  shared_ptr<Event> parsing_command = nullptr;

  size_t line_num = 0;
  size_t num_events = 0;
  while (!feof(input_log)) {
    line_num++;
    string line = fgets(input_log);
    if (starts_with(line, Shell::PROMPT)) {
      line = line.substr(Shell::PROMPT.size());
    }
    if (ends_with(line, "\n")) {
      line.resize(line.size() - 1);
    }
    if (line.empty()) {
      continue;
    }

    if (parsing_command.get()) {
      string expected_start = string_printf("%04zX |", parsing_command->data.size());
      if (starts_with(line, expected_start)) {
        // Parse out the hex part of the hex/ASCII dump
        string mask_bytes;
        string data_bytes = parse_data_string(
            line.substr(expected_start.size(), 16 * 3 + 1), &mask_bytes);
        parsing_command->data += data_bytes;
        parsing_command->mask += mask_bytes;
        continue;
      } else {
        if (parsing_command->type == Event::Type::RECEIVE) {
          this->apply_default_mask(parsing_command);
        } else if (parsing_command->type == Event::Type::SEND) {
          this->check_for_password(parsing_command);
        }
        parsing_command = nullptr;
      }
    }

    if (starts_with(line, "I ")) {
      // I <pid/ts> - [Server] Client connected: C-%X on fd %d via %d (T-%hu-%s-%s-%s)
      // I <pid/ts> - [Server] Client connected: C-%X on virtual connection %p via T-%hu-VI
      size_t offset = line.find(" - [Server] Client connected: C-");
      if (offset != string::npos) {
        auto tokens = split(line, ' ');
        if (tokens.size() != 15) {
          throw runtime_error(string_printf("(ev-line %zu) client connection message has incorrect token count", line_num));
        }
        if (!starts_with(tokens[8], "C-")) {
          throw runtime_error(string_printf("(ev-line %zu) client connection message missing client ID token", line_num));
        }
        auto listen_tokens = split(tokens[14], '-');
        if (listen_tokens.size() < 4) {
          throw runtime_error(string_printf("(ev-line %zu) client connection message listening socket token format is incorrect", line_num));
        }

        shared_ptr<Client> c(new Client(
            this,
            stoull(tokens[8].substr(2), nullptr, 16),
            stoul(listen_tokens[1], nullptr, 10),
            version_for_name(listen_tokens[2].c_str())));
        if (!this->clients.emplace(c->id, c).second) {
          throw runtime_error(string_printf("(ev-line %zu) duplicate client ID in input log", line_num));
        }
        this->create_event(Event::Type::CONNECT, c, line_num);
        num_events++;
        continue;
      }

      // I <pid/ts> - [Server] Disconnecting C-%X on fd %d
      offset = line.find(" - [Server] Client disconnected: C-");
      if (offset != string::npos) {
        auto tokens = split(line, ' ');
        if (tokens.size() < 9) {
          throw runtime_error(string_printf("(ev-line %zu) client disconnection message has incorrect token count", line_num));
        }
        if (!starts_with(tokens[8], "C-")) {
          throw runtime_error(string_printf("(ev-line %zu) client disconnection message missing client ID token", line_num));
        }
        uint64_t client_id = stoul(tokens[8].substr(2), nullptr, 16);
        try {
          auto& c = this->clients.at(client_id);
          if (c->disconnect_event.get()) {
            throw runtime_error(string_printf("(ev-line %zu) client has multiple disconnect events", line_num));
          }
          c->disconnect_event = this->create_event(Event::Type::DISCONNECT, c, line_num);
          num_events++;
        } catch (const out_of_range&) {
          throw runtime_error(string_printf("(ev-line %zu) unknown disconnecting client ID in input log", line_num));
        }
        continue;
      }

      // I <pid/ts> - [Commands] Sending to C-%X (...)
      // I <pid/ts> - [Commands] Received from C-%X (...)
      offset = line.find(" - [Commands] Sending to C-");
      if (offset == string::npos) {
        offset = line.find(" - [Commands] Received from C-");
      }
      if (offset != string::npos) {
        auto tokens = split(line, ' ');
        if (tokens.size() < 10) {
          throw runtime_error(string_printf("(ev-line %zu) command header line too short", line_num));
        }
        bool from_client = (tokens[6] == "Received");
        uint64_t client_id = stoull(tokens[8].substr(2), nullptr, 16);
        try {
          parsing_command = this->create_event(
              from_client ? Event::Type::SEND : Event::Type::RECEIVE,
              this->clients.at(client_id),
              line_num);
          num_events++;
        } catch (const out_of_range&) {
          throw runtime_error(string_printf("(ev-line %zu) input log contains command for missing client", line_num));
        }
        continue;
      }
    }
  }

  replay_log.info("%zu clients in log", this->clients.size());
  for (const auto& it : this->clients) {
    string client_str = it.second->str();
    replay_log.info("  %" PRIu64 " => %s", it.first, client_str.c_str());
  }

  replay_log.info("%zu events in replay log", num_events);
  for (auto ev = this->first_event; ev != nullptr; ev = ev->next_event) {
    string ev_str = ev->str();
    replay_log.info("  %s", ev_str.c_str());
  }
}

void ReplaySession::start() {
  this->update_timeout_event();
  this->execute_pending_events();
}

void ReplaySession::update_timeout_event() {
  if (!this->timeout_ev.get()) {
    this->timeout_ev.reset(
        event_new(this->base.get(), -1, EV_TIMEOUT, this->dispatch_on_timeout, this),
        event_free);
  }
  struct timeval tv = usecs_to_timeval(3000000);
  event_add(this->timeout_ev.get(), &tv);
}

void ReplaySession::dispatch_on_timeout(evutil_socket_t, short, void*) {
  throw runtime_error("timeout waiting for next event");
}

void ReplaySession::execute_pending_events() {
  while (this->first_event) {
    if (!this->first_event->complete) {
      auto& c = this->clients.at(this->first_event->client_id);

      auto ev_str = this->first_event->str();
      replay_log.info("Event: %s", ev_str.c_str());

      switch (this->first_event->type) {
        case Event::Type::CONNECT: {
          if (c->channel.connected()) {
            throw runtime_error(string_printf("(ev-line %zu) connect event on already-connected client", this->first_event->line_num));
          }

          struct bufferevent* bevs[2];
          bufferevent_pair_new(this->base.get(), 0, bevs);

          c->channel.set_bufferevent(bevs[0]);
          this->channel_to_client.emplace(&c->channel, c);

          shared_ptr<const PortConfiguration> port_config;
          try {
            port_config = this->state->number_to_port_config.at(c->port);
          } catch (const out_of_range&) {
            bufferevent_free(bevs[1]);
            throw runtime_error(string_printf("(ev-line %zu) client connected to port missing from configuration", this->first_event->line_num));
          }

          if (port_config->behavior == ServerBehavior::PROXY_SERVER) {
            // TODO: We should support this at some point in the future
            throw runtime_error(string_printf("(ev-line %zu) client connected to proxy server", this->first_event->line_num));
          } else if (this->state->game_server.get()) {
            this->state->game_server->connect_client(bevs[1], 0x20202020,
                1025, c->port, port_config->version, port_config->behavior);
          } else {
            throw runtime_error(string_printf("(ev-line %zu) no server available for connection", this->first_event->line_num));
            bufferevent_free(bevs[1]);
          }
          break;
        }
        case Event::Type::DISCONNECT:
          this->channel_to_client.erase(&c->channel);
          c->channel.disconnect();
          break;
        case Event::Type::SEND:
          if (!c->channel.connected()) {
            throw runtime_error(string_printf("(ev-line %zu) send event attempted on unconnected client", this->first_event->line_num));
          }
          c->channel.send(this->first_event->data);
          this->commands_sent++;
          this->bytes_sent += this->first_event->data.size();
          break;
        case Event::Type::RECEIVE:
          // Receive events cannot be executed here, since we have to wait for
          // an incoming command. The existing handlers will take care of it:
          // on_command_received will be called sometime (hopefully) soon.
          return;
        default:
          throw logic_error("unhandled event type");
      }
      this->first_event->complete = true;
    }

    this->first_event = this->first_event->next_event;
    if (!this->first_event.get()) {
      this->last_event = nullptr;
    }
  }

  // If we get here, then there are no more events to run: we're done.
  // TODO: We should flush any pending sends on the remaining client here, even
  // though there are no pending receives (to make sure the last sent commands
  // don't crash newserv)
  replay_log.info("Replay complete: %zu commands sent (%zu bytes), %zu commands received (%zu bytes)",
      this->commands_sent, this->bytes_sent, this->commands_received, this->bytes_received);
  event_base_loopexit(this->base.get(), nullptr);
}

void ReplaySession::dispatch_on_command_received(
    Channel& ch, uint16_t command, uint32_t flag, string& data) {
  ReplaySession* session = reinterpret_cast<ReplaySession*>(ch.context_obj);
  session->on_command_received(
      session->channel_to_client.at(&ch), command, flag, data);
}

void ReplaySession::dispatch_on_error(Channel& ch, short events) {
  ReplaySession* session = reinterpret_cast<ReplaySession*>(ch.context_obj);
  session->on_error(session->channel_to_client.at(&ch), events);
}

void ReplaySession::on_command_received(
    shared_ptr<Client> c, uint16_t command, uint32_t flag, string& data) {

  // TODO: Use the iovec form of print_data here instead of
  // prepend_command_header (which copies the string)
  string full_command = prepend_command_header(
      c->version, c->channel.crypt_in.get(), command, flag, data);
  this->commands_received++;
  this->bytes_received += full_command.size();

  if (c->receive_events.empty()) {
    print_data(stderr, full_command, 0, nullptr, PrintDataFlags::PRINT_ASCII | PrintDataFlags::OFFSET_16_BITS);
    throw runtime_error("received unexpected command for client");
  }

  auto& ev = c->receive_events.front();
  if ((full_command.size() != ev->data.size()) && !ev->allow_size_disparity) {
    replay_log.error("Expected command:");
    print_data(stderr, ev->data, 0, nullptr, PrintDataFlags::PRINT_ASCII | PrintDataFlags::OFFSET_16_BITS);
    replay_log.error("Received command:");
    print_data(stderr, full_command, 0, nullptr, PrintDataFlags::PRINT_ASCII | PrintDataFlags::OFFSET_16_BITS);
    throw runtime_error(string_printf("(ev-line %zu) received command sizes do not match", ev->line_num));
  }
  for (size_t x = 0; x < min<size_t>(full_command.size(), ev->data.size()); x++) {
    if ((full_command[x] & ev->mask[x]) != (ev->data[x] & ev->mask[x])) {
      replay_log.error("Expected command:");
      print_data(stderr, ev->data, 0, nullptr, PrintDataFlags::PRINT_ASCII | PrintDataFlags::OFFSET_16_BITS);
      replay_log.error("Received command:");
      print_data(stderr, full_command, 0, ev->data.data(), PrintDataFlags::PRINT_ASCII | PrintDataFlags::OFFSET_16_BITS);
      throw runtime_error(string_printf("(ev-line %zu) received command data does not match expected data", ev->line_num));
    }
  }

  ev->complete = true;
  c->receive_events.pop_front();

  // If the command is an encryption init, set up encryption on the channel
  switch (c->version) {
    case GameVersion::PATCH:
      if (command == 0x02) {
        auto& cmd = check_size_t<S_ServerInit_Patch_02>(data);
        c->channel.crypt_in.reset(new PSOV2Encryption(cmd.server_key));
        c->channel.crypt_out.reset(new PSOV2Encryption(cmd.client_key));
      }
      break;
    case GameVersion::DC:
    case GameVersion::PC:
    case GameVersion::GC:
    case GameVersion::XB:
      if (command == 0x02 || command == 0x17 || command == 0x91 || command == 0x9B) {
        auto& cmd = check_size_t<S_ServerInitDefault_DC_PC_V3_02_17_91_9B>(data, 0xFFFF);
        if ((c->version == GameVersion::DC) || (c->version == GameVersion::PC)) {
          c->channel.crypt_in.reset(new PSOV2Encryption(cmd.server_key));
          c->channel.crypt_out.reset(new PSOV2Encryption(cmd.client_key));
        } else { // V3
          c->channel.crypt_in.reset(new PSOV3Encryption(cmd.server_key));
          c->channel.crypt_out.reset(new PSOV3Encryption(cmd.client_key));
        }
      }
      break;
    case GameVersion::BB:
      if (command == 0x03 || command == 0x9B) {
        auto& cmd = check_size_t<S_ServerInitDefault_BB_03_9B>(data, 0xFFFF);
        // TODO: At some point it may matter which BB private key file we use.
        // Don't just blindly use the first one here.
        c->channel.crypt_in.reset(new PSOBBEncryption(
            *this->state->bb_private_keys[0], cmd.server_key.data(), cmd.server_key.size()));
        c->channel.crypt_out.reset(new PSOBBEncryption(
            *this->state->bb_private_keys[0], cmd.client_key.data(), cmd.client_key.size()));
      }
      break;
    default:
      throw logic_error("unsupported encryption version");
  }

  this->update_timeout_event();
  this->execute_pending_events();
}

void ReplaySession::on_error(shared_ptr<Client> c, short events) {
  if (events & BEV_EVENT_ERROR) {
    throw runtime_error(string_printf("C-%" PRIX64 " caused stream error", c->id));
  }
  if (events & BEV_EVENT_EOF) {
    if (!c->disconnect_event.get()) {
      throw runtime_error(string_printf(
          "C-%" PRIX64 " disconnected, but has no disconnect event", c->id));
    }
    if (!c->receive_events.empty()) {
      throw runtime_error(string_printf(
          "C-%" PRIX64 " disconnected, but has pending receive events", c->id));
    }
    c->disconnect_event->complete = true;
    this->channel_to_client.erase(&c->channel);
    c->channel.disconnect();
  }
}
