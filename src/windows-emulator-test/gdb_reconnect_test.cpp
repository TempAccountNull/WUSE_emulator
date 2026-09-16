#include "emulation_test_utils.hpp"
#include <win_x86_64_gdb_stub_handler.hpp>
#include <connection_handler.hpp>
#include <network/tcp_server_socket.hpp>
#include <future>
#include <utils/finally.hpp>
#include <utils/string.hpp>

namespace sogen::test
{
    namespace
    {
        struct connected_pair
        {
            network::tcp_client_socket peer{AF_INET};
            network::tcp_client_socket target;

            connected_pair()
            {
                network::tcp_server_socket listener{AF_INET};
                if (!listener.bind(network::address{"127.0.0.1:0"sv, AF_INET}))
                {
                    throw std::runtime_error("Cannot bind test socket");
                }
                listener.listen();
                if (!peer.connect(*listener.get_name()))
                {
                    throw std::runtime_error("Cannot connect test socket");
                }
                target = listener.accept();
                peer.set_blocking(false);
            }
        };
    }

    TEST(GdbReconnect, DetachReplyIsFlushedBeforeClose)
    {
        connected_pair sockets;
        {
            gdb_stub::connection_handler connection{sockets.target};
            connection.send_raw_data("+");
            connection.send_reply("OK");
            connection.close_after_flush();
        }
        std::string received;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (sockets.peer.is_valid() && std::chrono::steady_clock::now() < deadline)
        {
            if (const auto part = sockets.peer.receive())
            {
                received += *part;
            }
        }
        EXPECT_EQ(received, "+$OK#9a");
        EXPECT_FALSE(sockets.peer.is_valid());
    }

    TEST(GdbReconnect, NonReadingPeerCannotBlockDetach)
    {
        connected_pair sockets;
        const int buffer_size = 4096;
        ASSERT_EQ(setsockopt(sockets.target.get_socket(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer_size),
                             sizeof(buffer_size)),
                  0);
        ASSERT_EQ(
            setsockopt(sockets.peer.get_socket(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size)),
            0);
        const auto started = std::chrono::steady_clock::now();
        {
            gdb_stub::connection_handler connection{sockets.target};
            connection.send_raw_data(std::string(16 * 1024 * 1024, 'x'));
            connection.close_after_flush();
        }
        EXPECT_FALSE(sockets.target.is_valid());
        EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
    }

    TEST(GdbReconnect, DetachDiscardsPipelinedMutations)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto win_emu = create_sample_emulator(std::move(settings));
        win_emu.setup_process_if_necessary();
        const auto address = win_emu.mod_manager.executable->entry_point;
        win_emu.emu().write_memory<uint8_t>(address, 0x90);
        network::tcp_server_socket reservation{AF_INET};
        ASSERT_TRUE(reservation.bind(network::address{"127.0.0.1:0"sv, AF_INET}));
        const auto endpoint = *reservation.get_name();
        reservation.close();
        std::atomic_bool stop{};
        win_x86_64_gdb_stub_handler handler{win_emu, [&] { return stop.load(); }};
        gdb_stub::session_end_reason reason{};
        auto server = std::async(std::launch::async, [&] { return gdb_stub::run_gdb_stub(endpoint, handler, &reason); });
        const auto cleanup = utils::finally([&] { stop = true; });
        network::tcp_client_socket client;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!client.is_valid() && std::chrono::steady_clock::now() < deadline)
        {
            network::tcp_client_socket candidate{AF_INET};
            if (candidate.connect(endpoint))
            {
                client = std::move(candidate);
            }
            else
            {
                std::this_thread::sleep_for(10ms);
            }
        }
        ASSERT_TRUE(client.is_valid());
        const auto packet = [](const std::string& payload) {
            uint8_t checksum{};
            for (const auto byte : payload)
            {
                checksum += static_cast<uint8_t>(byte);
            }
            return "$" + payload + "#" + utils::string::to_hex_string(checksum);
        };
        ASSERT_TRUE(client.send(packet("D") + packet("M" + utils::string::to_hex_number(address) + ",1:cc")));
        ASSERT_EQ(server.wait_for(3s), std::future_status::ready);
        EXPECT_TRUE(server.get());
        EXPECT_EQ(reason, gdb_stub::session_end_reason::detached);
        EXPECT_EQ(win_emu.emu().read_memory<uint8_t>(address), 0x90);
    }

    TEST(GdbReconnect, ReplacingHandlerPreservesGuestAndRemovesSessionHooks)
    {
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        auto win_emu = create_sample_emulator(std::move(settings));
        win_emu.setup_process_if_necessary();
        const auto address = win_emu.mod_manager.executable->entry_point;
        const std::array<uint8_t, 4> code{0x90, 0x90, 0x90, 0x90};
        win_emu.emu().write_memory(address, code.data(), code.size());
        win_emu.emu().reg(x86_register::rip, address);
        win_emu.emu().reg(x86_register::r12, 0x1122334455667788ull);
        const auto registers = win_emu.emu().save_registers();
        const auto instructions = win_emu.get_executed_instructions();
        size_t printed{};
        win_emu.callbacks.on_debug_string.add([&](std::string_view) { ++printed; });
        {
            win_x86_64_gdb_stub_handler previous{win_emu};
            ASSERT_TRUE(previous.set_breakpoint(gdb_stub::breakpoint_type::software, address + 1, 1));
            previous.on_interrupt();
        }
        EXPECT_EQ(win_emu.emu().save_registers(), registers);
        EXPECT_EQ(win_emu.get_executed_instructions(), instructions);
        std::array<uint8_t, 4> restored{};
        win_emu.emu().read_memory(address, restored.data(), restored.size());
        EXPECT_EQ(restored, code);

        win_x86_64_gdb_stub_handler current{win_emu};
        EXPECT_FALSE(current.delete_breakpoint(gdb_stub::breakpoint_type::software, address + 1, 1));
        EXPECT_EQ(current.singlestep(), gdb_stub::action::resume);
        EXPECT_EQ(current.singlestep(), gdb_stub::action::resume);
        EXPECT_EQ(win_emu.emu().read_instruction_pointer(), address + 2);
        EXPECT_EQ(win_emu.get_executed_instructions(), instructions + 2);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::r12), 0x1122334455667788ull);
        win_emu.callbacks.on_debug_string("output after reconnect");
        current.on_interrupt();
        EXPECT_EQ(current.run(), gdb_stub::action::output);
        EXPECT_EQ(current.consume_debug_output(), "output after reconnect");
        EXPECT_EQ(printed, 1u);
        EXPECT_EQ(current.run(), gdb_stub::action::resume);
        EXPECT_EQ(win_emu.get_executed_instructions(), instructions + 2);
    }
}
