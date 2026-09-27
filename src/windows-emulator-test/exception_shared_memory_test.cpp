#include "../windows-emulator/exception_shared_memory.hpp"

#include <gtest/gtest.h>
#include <utils/finally.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
namespace sogen::test
{
    namespace
    {
        struct mapped_exceptions
        {
            HANDLE handle{};
            const unsigned char* view{};

            explicit mapped_exceptions(const wchar_t* base_name)
            {
                const auto name = std::wstring(L"Local\\") + base_name + std::to_wstring(GetCurrentProcessId());
                this->handle = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
                if (this->handle)
                {
                    this->view = static_cast<const unsigned char*>(
                        MapViewOfFile(this->handle, FILE_MAP_READ, 0, 0, detail::exception_shared_memory::mapping_size));
                }
            }

            ~mapped_exceptions()
            {
                if (this->view)
                {
                    UnmapViewOfFile(this->view);
                }
                if (this->handle)
                {
                    CloseHandle(this->handle);
                }
            }

            mapped_exceptions(const mapped_exceptions&) = delete;
            mapped_exceptions& operator=(const mapped_exceptions&) = delete;

            [[nodiscard]] detail::exception_shared_header header() const
            {
                detail::exception_shared_header result{};
                std::memcpy(&result, this->view, sizeof(result));
                return result;
            }

            [[nodiscard]] detail::exception_shared_packet slot(size_t index) const
            {
                detail::exception_shared_packet result{};
                std::memcpy(&result, this->view + sizeof(detail::exception_shared_header) +
                                        index * sizeof(detail::exception_shared_packet), sizeof(result));
                return result;
            }
        };
    }

    TEST(ExceptionSharedMemory, PublishesExactVersionedPacketAndReleasesMapping)
    {
        constexpr auto base = L"SogenExceptions-";
        const auto name = std::wstring(L"Local\\") + base + std::to_wstring(GetCurrentProcessId());
        {
            detail::exception_shared_memory publisher(base);
            detail::exception_shared_packet packet{};
            packet.ordinal = 1;
            packet.non_debug_total = 1;
            packet.status = 0xC0000005;
            packet.tid = 32;
            packet.vcpu = 2;
            packet.rip = 0x140012345;
            packet.info = 0x1234;
            packet.module_base = 0x140000000;
            packet.module_rva = 0x12345;
            std::memcpy(packet.module_name.data(), "destiny2.exe", sizeof("destiny2.exe"));
            packet.code_bytes[0] = 0x48;
            packet.code_bytes[1] = 0x8B;
            packet.readable_code_bytes = 2;
            packet.gprs[0] = 0xAA;
            packet.gprs[7] = 0x10000;
            packet.stack_words[0] = 0xDEADBEEF;
            packet.readable_stack_words = 1;
            ASSERT_TRUE(publisher.publish(packet));

            mapped_exceptions mapped(base);
            ASSERT_NE(mapped.handle, nullptr);
            ASSERT_NE(mapped.view, nullptr);
            const auto header = mapped.header();
            EXPECT_EQ(header.magic, 0x58454753u);
            EXPECT_EQ(header.version, 1u);
            EXPECT_EQ(header.header_bytes, 64u);
            EXPECT_EQ(header.slot_bytes, 448u);
            EXPECT_EQ(header.capacity, 128u);
            EXPECT_EQ(header.writer_sequence, 2);
            EXPECT_EQ(header.total_published, 1u);
            EXPECT_EQ(header.last_observed_ordinal, 1u);
            EXPECT_EQ(header.non_debug_total, 1u);
            const auto actual = mapped.slot(0);
            EXPECT_EQ(actual.published_sequence, 1u);
            EXPECT_EQ(actual.ordinal, 1u);
            EXPECT_EQ(actual.status, 0xC0000005u);
            EXPECT_EQ(actual.rip, 0x140012345u);
            EXPECT_EQ(actual.module_rva, 0x12345u);
            EXPECT_STREQ(actual.module_name.data(), "destiny2.exe");
            EXPECT_EQ(actual.code_bytes[1], 0x8Bu);
            EXPECT_EQ(actual.readable_code_bytes, 2u);
            EXPECT_EQ(actual.gprs[0], 0xAAu);
            EXPECT_EQ(actual.stack_words[0], 0xDEADBEEFu);
            EXPECT_EQ(actual.readable_stack_words, 1u);
            // Opt-in cross-process wire check: keep the C++ producer alive
            // briefly while the Python MCP reader opens this named mapping.
            if (const auto* hold = std::getenv("SOGEN_EXCEPTION_INTEROP_HOLD"); hold && std::strcmp(hold, "1") == 0)
            {
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
        }
        auto* const released = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        EXPECT_EQ(released, nullptr);
        if (released)
        {
            CloseHandle(released);
        }
    }

    TEST(ExceptionSharedMemory, WrapsAtCapacityAndTracksSampledOutOrdinal)
    {
        constexpr auto base = L"SogenExceptions-WrapTest-";
        detail::exception_shared_memory publisher(base);
        for (uint64_t ordinal = 1; ordinal <= 129; ++ordinal)
        {
            detail::exception_shared_packet packet{};
            packet.ordinal = ordinal;
            packet.non_debug_total = ordinal;
            packet.rip = ordinal;
            ASSERT_TRUE(publisher.publish(packet));
        }
        ASSERT_TRUE(publisher.observe_skipped());

        mapped_exceptions mapped(base);
        ASSERT_NE(mapped.view, nullptr);
        const auto header = mapped.header();
        EXPECT_EQ(header.total_published, 129u);
        EXPECT_EQ(header.last_observed_ordinal, 130u);
        EXPECT_EQ(header.non_debug_total, 129u);
        EXPECT_EQ(header.debug_total, 1u);
        EXPECT_EQ(header.writer_sequence, 260);
        EXPECT_EQ(mapped.slot(0).published_sequence, 129u);
        EXPECT_EQ(mapped.slot(0).ordinal, 129u);
        EXPECT_EQ(mapped.slot(1).published_sequence, 2u);
        EXPECT_EQ(mapped.slot(1).ordinal, 2u);
    }

    TEST(ExceptionSharedMemory, TwoEmulatorOrdinalStreamsUseProcessGlobalWireCounters)
    {
        constexpr auto base = L"SogenExceptions-MultiInstanceTest-";
        detail::exception_shared_memory publisher(base);
        detail::exception_shared_packet first{};
        first.ordinal = 1; // first emulator's local ordinal
        first.non_debug_total = 1;
        first.status = 0xC0000005;
        first.rip = 0x1000;
        ASSERT_TRUE(publisher.publish(first));
        ASSERT_TRUE(publisher.observe_skipped()); // sampled-out debug event

        detail::exception_shared_packet second{};
        second.ordinal = 1; // second emulator restarts its local ordinal
        second.non_debug_total = 1;
        second.status = 0xC000001D;
        second.rip = 0x2000;
        ASSERT_TRUE(publisher.publish(second));

        mapped_exceptions mapped(base);
        ASSERT_NE(mapped.view, nullptr);
        const auto header = mapped.header();
        EXPECT_EQ(header.total_published, 2u);
        EXPECT_EQ(header.last_observed_ordinal, 3u);
        EXPECT_EQ(header.non_debug_total, 2u);
        EXPECT_EQ(header.debug_total, 1u);
        EXPECT_GE(header.last_observed_ordinal, header.non_debug_total + header.debug_total);
        EXPECT_GE(header.last_observed_ordinal, header.total_published);
        const auto first_wire = mapped.slot(0);
        const auto second_wire = mapped.slot(1);
        EXPECT_EQ(first_wire.published_sequence, 1u);
        EXPECT_EQ(first_wire.ordinal, 1u);
        EXPECT_EQ(second_wire.published_sequence, 2u);
        EXPECT_EQ(second_wire.ordinal, 3u);
        EXPECT_EQ(second_wire.non_debug_total, 2u);
        EXPECT_EQ(second_wire.debug_total, 1u);
        EXPECT_EQ(second_wire.rip, 0x2000u);
    }

    TEST(ExceptionSharedMemory, ConcurrentReaderNeverAcceptsTornSnapshot)
    {
        constexpr auto base = L"SogenExceptions-ConcurrentTest-";
        detail::exception_shared_memory publisher(base);
        detail::exception_shared_packet first{};
        first.ordinal = first.non_debug_total = 1;
        first.rip = 1;
        first.gprs.fill(1);
        first.stack_words.fill(1);
        ASSERT_TRUE(publisher.publish(first));
        mapped_exceptions mapped(base);
        ASSERT_NE(mapped.view, nullptr);

        std::atomic_bool started{false};
        std::atomic_bool finished{false};
        std::thread writer([&] {
            started.store(true);
            for (uint64_t ordinal = 2; ordinal <= 1000; ++ordinal)
            {
                detail::exception_shared_packet packet{};
                packet.ordinal = packet.non_debug_total = ordinal;
                packet.rip = ordinal;
                packet.gprs.fill(ordinal);
                packet.stack_words.fill(ordinal);
                (void)publisher.publish(packet);
                if (ordinal % 8 == 0)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(20));
                }
            }
            finished.store(true);
        });
        while (!started.load())
        {
            std::this_thread::yield();
        }

        size_t accepted = 0;
        while (!finished.load() || accepted == 0)
        {
            std::array<unsigned char, detail::exception_shared_memory::mapping_size> copy{};
            const auto* const sequence_address = reinterpret_cast<const volatile LONG*>(
                mapped.view + offsetof(detail::exception_shared_header, writer_sequence));
            const auto before_sequence = *sequence_address;
            if (before_sequence & 1)
            {
                continue;
            }
            MemoryBarrier();
            std::memcpy(copy.data(), mapped.view, copy.size());
            MemoryBarrier();
            const auto after_sequence = *sequence_address;
            detail::exception_shared_header snapshot{};
            std::memcpy(&snapshot, copy.data(), sizeof(snapshot));
            if (before_sequence != after_sequence || after_sequence & 1 ||
                snapshot.writer_sequence != before_sequence)
            {
                continue;
            }
            const auto oldest = std::max<uint64_t>(1, snapshot.total_published -
                                                     std::min<uint64_t>(snapshot.total_published, 128) + 1);
            for (uint64_t sequence = oldest; sequence <= snapshot.total_published; ++sequence)
            {
                detail::exception_shared_packet packet{};
                const auto index = (sequence - 1) % detail::exception_shared_memory::capacity;
                std::memcpy(&packet, copy.data() + sizeof(detail::exception_shared_header) +
                                        index * sizeof(packet), sizeof(packet));
                EXPECT_EQ(packet.published_sequence, sequence);
                EXPECT_EQ(packet.ordinal, packet.rip);
                EXPECT_EQ(packet.gprs[0], packet.ordinal);
                EXPECT_EQ(packet.stack_words[15], packet.ordinal);
            }
            ++accepted;
        }
        writer.join();
        EXPECT_GT(accepted, 0u);
    }
}
#endif
