#include "emulation_test_utils.hpp"
#include <connection_handler.hpp>
#include <win_x86_64_gdb_stub_handler.hpp>
#include <network/tcp_server_socket.hpp>
#include <utils/finally.hpp>
#include <utils/string.hpp>
#include <future>

namespace sogen::test
{
    namespace
    {
        using test_clock = std::chrono::steady_clock;

        template <typename Predicate>
        bool wait_until(const test_clock::time_point deadline, Predicate&& predicate)
        {
            while (!predicate())
            {
                if (test_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(5ms);
            }
            return true;
        }

        std::string packet(const std::string_view payload)
        {
            uint8_t checksum{};
            for (const auto byte : payload)
            {
                checksum += static_cast<uint8_t>(byte);
            }
            return "$" + std::string(payload) + "#" + utils::string::to_hex_string(checksum);
        }

        network::address reserve_endpoint()
        {
            network::tcp_server_socket reservation{AF_INET};
            if (!reservation.bind(network::address{"127.0.0.1:0"sv, AF_INET}))
            {
                throw std::runtime_error("Cannot reserve idle test socket");
            }
            return *reservation.get_name();
        }

        network::tcp_client_socket connect_peer(const network::address& endpoint, const test_clock::time_point deadline)
        {
            network::tcp_client_socket client;
            wait_until(deadline, [&] {
                network::tcp_client_socket candidate{AF_INET};
                if (candidate.connect(endpoint))
                {
                    client = std::move(candidate);
                    client.set_blocking(false);
                }
                return client.is_valid();
            });
            return client;
        }

        bool receive_until(network::tcp_client_socket& client, const std::string_view expected, const test_clock::time_point deadline)
        {
            std::string received;
            const auto completed = wait_until(deadline, [&] {
                if (const auto data = client.receive())
                {
                    received += *data;
                }
                return received.find(expected) != std::string::npos || !client.is_valid();
            });
            return completed && received.find(expected) != std::string::npos;
        }

        struct ui_observations
        {
            const std::thread::id owner{std::this_thread::get_id()};
            std::atomic_size_t pumps{};
            std::atomic_bool wrong_thread{};
            std::atomic_bool destroyed{};
            std::function<void()> on_pump;
        };

        class idle_ui_backend final : public ui_backend
        {
          public:
            explicit idle_ui_backend(std::shared_ptr<ui_observations> observations)
                : observations_(std::move(observations))
            {
            }

            ~idle_ui_backend() override
            {
                this->check_owner();
                this->observations_->destroyed = true;
            }

            void set_event_sink(event_sink /*sink*/) override
            {
                this->check_owner();
            }

            void pump_events() override
            {
                this->check_owner();
                ++this->observations_->pumps;
                if (this->observations_->on_pump)
                {
                    this->observations_->on_pump();
                }
            }

          private:
            std::shared_ptr<ui_observations> observations_;

            void check_owner() const
            {
                if (std::this_thread::get_id() != this->observations_->owner)
                {
                    this->observations_->wrong_thread = true;
                }
            }
        };

        windows_emulator make_idle_emulator(const std::shared_ptr<ui_observations>& observations)
        {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            emulator_interfaces interfaces;
            interfaces.ui = std::make_unique<idle_ui_backend>(observations);
            return create_sample_emulator(std::move(settings), {}, {}, std::move(interfaces));
        }

        struct connected_sockets
        {
            network::tcp_client_socket peer{AF_INET};
            network::tcp_client_socket target;

            connected_sockets()
            {
                network::tcp_server_socket listener{AF_INET};
                if (!listener.bind(network::address{"127.0.0.1:0"sv, AF_INET}))
                {
                    throw std::runtime_error("Cannot bind idle test socket");
                }
                listener.listen();
                if (!this->peer.connect(*listener.get_name()))
                {
                    throw std::runtime_error("Cannot connect idle test socket");
                }
                this->target = listener.accept();
            }
        };

        class interrupt_only_handler final : public win_x86_64_gdb_stub_handler
        {
          public:
            using win_x86_64_gdb_stub_handler::win_x86_64_gdb_stub_handler;
            std::atomic_bool entered_run{};
            std::atomic_bool interrupted{};
            std::thread::id run_thread;
            std::thread::id interrupt_thread;

            gdb_stub::action run() override
            {
                this->run_thread = std::this_thread::get_id();
                this->entered_run = true;
                // Exercise the actual asynchronous interrupt monitor without starting a CPU backend.
                wait_until(test_clock::now() + 5s, [&] { return this->interrupted.load() || this->should_stop(); });
                return gdb_stub::action::resume;
            }

            void on_interrupt() override
            {
                this->interrupt_thread = std::this_thread::get_id();
                this->interrupted = true;
            }
        };

        class observed_stop_handler final : public win_x86_64_gdb_stub_handler
        {
          public:
            using win_x86_64_gdb_stub_handler::win_x86_64_gdb_stub_handler;
            using x86_64_gdb_stub_handler::record_watchpoint;
            size_t resumes{};

            gdb_stub::action run() override
            {
                ++this->resumes;
                return gdb_stub::action::resume;
            }

            gdb_stub::action singlestep() override
            {
                ++this->resumes;
                return gdb_stub::action::resume;
            }

            std::vector<gdb_stub::thread_info> get_thread_list() const override
            {
                // Watchpoint byte values are hex-encoded. A thread name independently exercises
                // the shared qXfer XML escaping and RSP binary-escaping path with reserved bytes.
                return {{.id = 1, .name = "watchpoint $#}* <&\"'>"}};
            }
        };

        class rsp_test_peer
        {
          public:
            rsp_test_peer(network::tcp_client_socket& socket, const test_clock::time_point deadline)
                : socket_(socket),
                  deadline_(deadline)
            {
            }

            bool saw_escape{};
            bool saw_run_length{};
            size_t chunks{};

            std::string request(const std::string_view command)
            {
                if (!this->socket_.send(packet(command)))
                {
                    throw std::runtime_error("Cannot send test RSP request");
                }
                std::optional<std::string> response;
                const auto ready = wait_until(this->deadline_, [&] {
                    if (const auto data = this->socket_.receive())
                    {
                        this->received_ += *data;
                    }
                    const auto begin = this->received_.find('$');
                    const auto end = this->received_.find('#', begin);
                    if (begin != std::string::npos && end != std::string::npos && this->received_.size() >= end + 3)
                    {
                        const auto wire = this->received_.substr(begin + 1, end - begin - 1);
                        uint8_t checksum{};
                        for (const auto byte : wire)
                        {
                            checksum += static_cast<uint8_t>(byte);
                        }
                        if (this->received_.substr(end + 1, 2) != utils::string::to_hex_string(checksum))
                        {
                            throw std::runtime_error("Bad test RSP response checksum");
                        }
                        response = this->decode(wire);
                        this->received_.erase(0, end + 3);
                    }
                    return response.has_value() || !this->socket_.is_valid();
                });
                if (!ready || !response)
                {
                    throw std::runtime_error("Missing test RSP response");
                }
                return *response;
            }

            std::string read_xfer(const std::string_view object)
            {
                std::string result;
                for (size_t part = 0; part < 128; ++part)
                {
                    const auto response =
                        this->request("qXfer:" + std::string(object) + ":read::" + utils::string::to_hex_number(result.size()) + ",2f");
                    if (response.empty() || (response.front() != 'm' && response.front() != 'l') || response.size() > 48)
                    {
                        throw std::runtime_error("Invalid or oversized test qXfer chunk");
                    }
                    ++this->chunks;
                    result += response.substr(1);
                    if (response.front() == 'l')
                    {
                        return result;
                    }
                    if (response.size() == 1)
                    {
                        throw std::runtime_error("Non-progressing test qXfer chunk");
                    }
                }
                throw std::runtime_error("Test qXfer chunk limit reached");
            }

          private:
            network::tcp_client_socket& socket_;
            test_clock::time_point deadline_;
            std::string received_;

            std::string decode(const std::string_view wire)
            {
                std::string expanded;
                for (size_t i = 0; i < wire.size(); ++i)
                {
                    if (wire[i] != '*')
                    {
                        expanded += wire[i];
                        continue;
                    }
                    if (expanded.empty())
                    {
                        throw std::runtime_error("Invalid test RSP run length");
                    }
                    ++i;
                    if (i == wire.size() || static_cast<uint8_t>(wire[i]) < 29)
                    {
                        throw std::runtime_error("Invalid test RSP run length");
                    }
                    this->saw_run_length = true;
                    expanded.append(static_cast<uint8_t>(wire[i]) - 29, expanded.back());
                }
                std::string decoded;
                for (size_t i = 0; i < expanded.size(); ++i)
                {
                    if (expanded[i] == '}')
                    {
                        this->saw_escape = true;
                        if (++i == expanded.size())
                        {
                            throw std::runtime_error("Truncated test RSP escape");
                        }
                        decoded += static_cast<char>(expanded[i] ^ 0x20);
                    }
                    else
                    {
                        decoded += expanded[i];
                    }
                }
                return decoded;
            }
        };
    }

    TEST(GdbUiIdle, ExistingConnectionCallerNeedsNoIdleCallback)
    {
        connected_sockets sockets;
        const auto deadline = test_clock::now() + 3s;
        gdb_stub::connection_handler connection{sockets.target, [&] { return test_clock::now() >= deadline; }};
        ASSERT_TRUE(sockets.peer.send(packet("?")));
        EXPECT_EQ(connection.get_packet(), "?");
    }

    TEST(GdbUiIdle, IdleStopPreventsReadingQueuedPacketAndRunsOnlyOnCaller)
    {
        connected_sockets sockets;
        const auto owner = std::this_thread::get_id();
        std::thread::id callback_thread;
        size_t callbacks{};
        bool stop{};
        ASSERT_TRUE(sockets.peer.send(packet("M1000,1:cc")));
        {
            gdb_stub::connection_handler connection{sockets.target, [&] { return stop; },
                                                    [&] {
                                                        callback_thread = std::this_thread::get_id();
                                                        ++callbacks;
                                                        stop = true;
                                                    }};
            EXPECT_FALSE(connection.get_packet().has_value());
        }
        EXPECT_EQ(callbacks, 1U);
        EXPECT_EQ(callback_thread, owner);
    }

    TEST(GdbUiIdle, IdleExceptionUnwindsConnectionWithoutWorkerCallback)
    {
        connected_sockets sockets;
        const auto owner = std::this_thread::get_id();
        std::thread::id callback_thread;
        const auto started = test_clock::now();
        EXPECT_THROW(([&] {
                         gdb_stub::connection_handler connection{sockets.target, {}, [&] {
                                                                     callback_thread = std::this_thread::get_id();
                                                                     throw std::runtime_error("idle callback failure");
                                                                 }};
                         (void)connection.get_packet();
                     }()),
                     std::runtime_error);
        EXPECT_EQ(callback_thread, owner);
        EXPECT_LT(test_clock::now() - started, 3s);
    }

    TEST(GdbUiIdle, AcceptPumpsOnUiOwnerAndStopsWithoutExecutingGuest)
    {
        const auto observations = std::make_shared<ui_observations>();
        {
            auto win = make_idle_emulator(observations);
            win.setup_process_if_necessary();
            const auto registers = win.emu().save_registers();
            const auto instructions = win.get_executed_instructions();
            const auto initial_pumps = observations->pumps.load();
            std::atomic_bool stop{};
            observations->on_pump = [&] {
                if (observations->pumps >= initial_pumps + 3)
                {
                    stop = true;
                }
            };
            const auto deadline = test_clock::now() + 3s;
            win_x86_64_gdb_stub_handler handler{win, [&] { return stop.load() || test_clock::now() >= deadline; }};
            gdb_stub::session_end_reason reason{};
            EXPECT_FALSE(gdb_stub::run_gdb_stub(reserve_endpoint(), handler, &reason));
            EXPECT_EQ(reason, gdb_stub::session_end_reason::stop_requested);
            EXPECT_EQ(observations->pumps.load(), initial_pumps + 3);
            EXPECT_EQ(win.emu().save_registers(), registers);
            EXPECT_EQ(win.get_executed_instructions(), instructions);
            observations->on_pump = {};
        }
        EXPECT_TRUE(observations->destroyed.load());
        EXPECT_FALSE(observations->wrong_thread.load());
    }

    TEST(GdbUiIdle, PausedDisconnectAndReconnectPreserveGuestAndUiOwner)
    {
        const auto observations = std::make_shared<ui_observations>();
        {
            auto win = make_idle_emulator(observations);
            win.setup_process_if_necessary();
            const auto registers = win.emu().save_registers();
            const auto instructions = win.get_executed_instructions();
            for (const bool detach : {false, true})
            {
                const auto endpoint = reserve_endpoint();
                const auto deadline = test_clock::now() + 5s;
                const auto initial_pumps = observations->pumps.load();
                std::atomic_bool stop{};
                win_x86_64_gdb_stub_handler handler{win, [&] { return stop.load() || test_clock::now() >= deadline; }};
                auto peer = std::async(std::launch::async, [&] {
                    if (!wait_until(deadline, [&] { return observations->pumps > initial_pumps; }))
                    {
                        return false;
                    }
                    auto client = connect_peer(endpoint, deadline);
                    if (!client || !client.send(packet("?")) || !receive_until(client, "$T05", deadline))
                    {
                        return false;
                    }
                    const auto connected_pumps = observations->pumps.load();
                    if (!wait_until(deadline, [&] { return observations->pumps >= connected_pumps + 2; }))
                    {
                        return false;
                    }
                    if (detach)
                    {
                        return client.send(packet("D")) && receive_until(client, "$OK#", deadline);
                    }
                    client.close();
                    return true;
                });
                const auto cleanup = utils::finally([&] { stop = true; });
                gdb_stub::session_end_reason reason{};
                EXPECT_TRUE(gdb_stub::run_gdb_stub(endpoint, handler, &reason));
                EXPECT_TRUE(peer.get());
                EXPECT_EQ(reason, detach ? gdb_stub::session_end_reason::detached : gdb_stub::session_end_reason::transport_closed);
                EXPECT_EQ(win.emu().save_registers(), registers);
                EXPECT_EQ(win.get_executed_instructions(), instructions);
                EXPECT_FALSE(observations->wrong_thread.load());
            }
        }
        EXPECT_TRUE(observations->destroyed.load());
        EXPECT_FALSE(observations->wrong_thread.load());
    }

    TEST(GdbUiIdle, InterruptMonitorUsesStopPredicateWithoutPumpingUi)
    {
        const auto observations = std::make_shared<ui_observations>();
        auto win = make_idle_emulator(observations);
        win.setup_process_if_necessary();
        const auto registers = win.emu().save_registers();
        const auto instructions = win.get_executed_instructions();
        const auto endpoint = reserve_endpoint();
        const auto deadline = test_clock::now() + 5s;
        const auto initial_pumps = observations->pumps.load();
        std::atomic_bool stop{};
        std::atomic_size_t worker_stop_checks{};
        interrupt_only_handler handler{win, [&] {
                                           if (std::this_thread::get_id() != observations->owner)
                                           {
                                               ++worker_stop_checks;
                                           }
                                           return stop.load() || test_clock::now() >= deadline;
                                       }};
        auto peer = std::async(std::launch::async, [&] {
            if (!wait_until(deadline, [&] { return observations->pumps > initial_pumps; }))
            {
                return false;
            }
            auto client = connect_peer(endpoint, deadline);
            if (!client || !client.send(packet("c")) || !wait_until(deadline, [&] { return handler.entered_run.load(); }) ||
                !wait_until(deadline, [&] { return worker_stop_checks.load() > 0; }))
            {
                return false;
            }
            if (!client.send("\x03"sv) || !receive_until(client, "$T05", deadline))
            {
                return false;
            }
            return client.send(packet("D")) && receive_until(client, "$OK#", deadline);
        });
        const auto cleanup = utils::finally([&] { stop = true; });
        gdb_stub::session_end_reason reason{};
        EXPECT_TRUE(gdb_stub::run_gdb_stub(endpoint, handler, &reason));
        EXPECT_TRUE(peer.get());
        EXPECT_EQ(reason, gdb_stub::session_end_reason::detached);
        EXPECT_GT(worker_stop_checks.load(), 0U);
        EXPECT_TRUE(handler.interrupted.load());
        EXPECT_EQ(handler.run_thread, observations->owner);
        EXPECT_NE(handler.interrupt_thread, observations->owner);
        EXPECT_FALSE(observations->wrong_thread.load());
        EXPECT_EQ(win.emu().save_registers(), registers);
        EXPECT_EQ(win.get_executed_instructions(), instructions);
    }

    TEST(GdbUiIdle, SocketWatchpointXferAdvertisesChunksAndPreservesImmutableStopWithoutResume)
    {
        const auto observations = std::make_shared<ui_observations>();
        auto win = make_idle_emulator(observations);
        win.setup_process_if_necessary();
        const auto endpoint = reserve_endpoint();
        const auto deadline = test_clock::now() + 10s;
        const auto initial_pumps = observations->pumps.load();
        std::atomic_bool stop{};
        observed_stop_handler handler{win, [&] { return stop.load() || test_clock::now() >= deadline; }};
        const auto callback_pc = win.mod_manager.executable->entry_point;
        win.emu().reg(x86_register::rip, callback_pc);
        std::array<uint8_t, 64> value{};
        value[0] = '$';
        value[1] = '#';
        value[2] = '}';
        value[3] = '*';
        // This is an injected failed-access observation, not proof that a native/guest store ran.
        handler.record_watchpoint(win.emu(), 0x12340004, 4, 0x12340000, value.data(), value.size(), true,
                                  {.outcome = memory_access_outcome::failed, .backend_error = 0x17});
        value.fill(0xCC);
        win.emu().reg(x86_register::rip, callback_pc + 1);
        const auto captured = handler.get_watchpoint_observations();
        const auto expected_xml = gdb_stub::format_watchpoint_observations(captured);
        ASSERT_NE(expected_xml.find("outcome=\"failed\""), std::string::npos);
        ASSERT_NE(expected_xml.find("value-kind=\"attempted\""), std::string::npos);
        ASSERT_NE(expected_xml.find("value=\"24237d2a00000000"), std::string::npos);
        const auto registers = win.emu().save_registers();
        const auto instructions = win.get_executed_instructions();

        struct exchange_result
        {
            std::string supported;
            std::string first;
            std::string second;
            std::string threads;
            std::string eof;
            std::string detached;
            size_t chunks{};
            bool escaped{};
            bool run_length{};
        };

        auto peer = std::async(std::launch::async, [&] {
            if (!wait_until(deadline, [&] { return observations->pumps > initial_pumps; }))
            {
                throw std::runtime_error("GDB caller did not reach its accept loop");
            }
            auto client = connect_peer(endpoint, deadline);
            if (!client)
            {
                throw std::runtime_error("Cannot connect watchpoint RSP test peer");
            }
            rsp_test_peer remote{client, deadline};
            exchange_result result;
            result.supported = remote.request("qSupported");
            result.first = remote.read_xfer("sogen-watchpoints");
            result.eof = remote.request("qXfer:sogen-watchpoints:read::" + utils::string::to_hex_number(result.first.size()) + ",2f");
            result.threads = remote.read_xfer("threads");
            result.second = remote.read_xfer("sogen-watchpoints");
            result.detached = remote.request("D");
            result.chunks = remote.chunks;
            result.escaped = remote.saw_escape;
            result.run_length = remote.saw_run_length;
            return result;
        });
        const auto cleanup = utils::finally([&] { stop = true; });
        gdb_stub::session_end_reason reason{};
        EXPECT_TRUE(gdb_stub::run_gdb_stub(endpoint, handler, &reason));
        const auto result = peer.get();
        EXPECT_NE(result.supported.find("qXfer:sogen-watchpoints:read+"), std::string::npos);
        EXPECT_EQ(result.first, expected_xml);
        EXPECT_EQ(result.second, expected_xml);
        EXPECT_EQ(result.eof, "l");
        EXPECT_NE(result.threads.find("watchpoint $#}* &lt;&amp;&quot;'&gt;"), std::string::npos);
        EXPECT_TRUE(result.escaped);
        EXPECT_TRUE(result.run_length);
        EXPECT_GT(result.chunks, 4U);
        EXPECT_EQ(result.detached, "OK");
        EXPECT_EQ(reason, gdb_stub::session_end_reason::detached);
        EXPECT_EQ(handler.resumes, 0U);
        EXPECT_EQ(gdb_stub::format_watchpoint_observations(handler.get_watchpoint_observations()), expected_xml);
        EXPECT_EQ(win.emu().save_registers(), registers);
        EXPECT_EQ(win.get_executed_instructions(), instructions);
        EXPECT_FALSE(observations->wrong_thread.load());
    }
}
