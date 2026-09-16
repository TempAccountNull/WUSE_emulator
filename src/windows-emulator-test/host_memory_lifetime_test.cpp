#include <gtest/gtest.h>
#include <memory_manager.hpp>

#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr size_t page = 0x1000;
        constexpr uint64_t source = 0x70000000;
        constexpr uint64_t view = 0x71000000;

        struct host_storage
        {
            explicit host_storage(std::shared_ptr<size_t> destruction_count)
                : destroyed(std::move(destruction_count)),
                  bytes(page * 3)
            {
            }

            ~host_storage()
            {
                ++*this->destroyed;
            }

            std::shared_ptr<size_t> destroyed;
            std::vector<uint8_t> bytes;
        };

        class lifetime_memory : public memory_interface
        {
          public:
            bool fail_host_map{};
            size_t fail_shared_call{};
            size_t shared_calls{};
            std::optional<uint64_t> fail_unmap{};
            std::vector<uint64_t> unmap_attempts;
            size_t claims{};
            size_t claim_releases{};
            std::map<uint64_t, uint8_t*> pages;
            std::map<uint64_t, std::vector<uint8_t>> ordinary;

            void reserve_guest_address_range(uint64_t, size_t) override
            {
                ++this->claims;
            }

            void release_guest_address_range(uint64_t, size_t) override
            {
                ++this->claim_releases;
            }

            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!this->try_read_memory(address, data, size))
                {
                    throw std::runtime_error("unmapped read");
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                auto* output = static_cast<uint8_t*>(data);
                for (size_t i = 0; i < size; ++i)
                {
                    const auto entry = this->pages.find((address + i) & ~(uint64_t{page} - 1));
                    if (entry == this->pages.end())
                    {
                        return false;
                    }
                    output[i] = entry->second[(address + i) % page];
                }
                return true;
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!this->try_write_memory(address, data, size))
                {
                    throw std::runtime_error("unmapped write");
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                const auto* input = static_cast<const uint8_t*>(data);
                for (size_t i = 0; i < size; ++i)
                {
                    const auto entry = this->pages.find((address + i) & ~(uint64_t{page} - 1));
                    if (entry == this->pages.end())
                    {
                        return false;
                    }
                    entry->second[(address + i) % page] = input[i];
                }
                return true;
            }

          private:
            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
                throw std::logic_error("unexpected MMIO callback mapping");
            }

            void map_memory(const uint64_t address, const size_t size, memory_permission) override
            {
                auto& bytes = this->ordinary[address];
                bytes.resize(size);
                for (size_t offset = 0; offset < size; offset += page)
                {
                    this->pages.emplace(address + offset, bytes.data() + offset);
                }
            }

            void map_host_memory(const uint64_t address, const size_t size, void* pointer, memory_permission) override
            {
                if (this->fail_host_map)
                {
                    throw std::runtime_error("injected host map failure");
                }
                for (size_t offset = 0; offset < size; offset += page)
                {
                    this->pages.emplace(address + offset, static_cast<uint8_t*>(pointer) + offset);
                }
            }

            bool map_shared_memory(const uint64_t address, const uint64_t backing, const size_t size, memory_permission) override
            {
                if (++this->shared_calls == this->fail_shared_call)
                {
                    throw std::runtime_error("injected shared map failure");
                }
                for (size_t offset = 0; offset < size; offset += page)
                {
                    this->pages.emplace(address + offset, this->pages.at(backing + offset));
                }
                return true;
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                this->unmap_attempts.push_back(address);
                if (this->fail_unmap == address)
                {
                    throw std::runtime_error("injected unmap failure");
                }
                for (size_t offset = 0; offset < size; offset += page)
                {
                    if (!this->pages.contains(address + offset))
                    {
                        throw std::runtime_error("attempt to unmap an unmapped page");
                    }
                }
                for (size_t offset = 0; offset < size; offset += page)
                {
                    this->pages.erase(address + offset);
                }
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
            }
        };
    }

    TEST(HostMemoryLifetimeTest, ViewsRetainStorageAfterOriginalMappingAndProducerRelease)
    {
        lifetime_memory backend;
        memory_manager memory{backend};
        auto destroyed = std::make_shared<size_t>();
        auto storage = std::make_shared<host_storage>(destroyed);
        ASSERT_TRUE(memory.allocate_host_memory_at(source, page * 3, storage->bytes.data(), memory_permission::read_write, storage));
        ASSERT_TRUE(memory.allocate_shared_view(view, source, page * 3, memory_permission::read_write));
        storage.reset();
        ASSERT_TRUE(memory.release_memory(source, 0));
        EXPECT_EQ(*destroyed, 0u);
        static_cast<memory_interface&>(memory).write_memory<uint64_t>(view + page, 0x123456789abcdef0);
        EXPECT_EQ(memory.read_memory<uint64_t>(view + page), 0x123456789abcdef0u);
        ASSERT_TRUE(memory.release_memory(view, 0));
        EXPECT_EQ(*destroyed, 1u);
    }

    TEST(HostMemoryLifetimeTest, RevocationTracksSplitViewsAndDoesNotRevokeReusedAddress)
    {
        lifetime_memory backend;
        memory_manager memory{backend};
        auto destroyed = std::make_shared<size_t>();
        auto storage = std::make_shared<host_storage>(destroyed);
        ASSERT_TRUE(memory.allocate_host_memory_at(source, page * 3, storage->bytes.data(), memory_permission::read_write, storage));
        auto token = memory.host_memory_backing_at(source);
        ASSERT_TRUE(memory.allocate_shared_view(view, source, page * 3, memory_permission::read_write));
        ASSERT_TRUE(memory.release_memory(view + page, page));
        EXPECT_EQ(memory.shared_view_source(view), source);
        EXPECT_EQ(memory.shared_view_source(view + page * 2), source + page * 2);
        EXPECT_EQ(memory.host_memory_backing_at(view + page * 2), token);
        ASSERT_TRUE(memory.release_memory(source, 0));
        ASSERT_TRUE(memory.allocate_memory(source, page * 3, memory_permission::read_write));
        storage.reset();
        memory.revoke_host_memory(token);
        EXPECT_FALSE(memory.get_region_info(view).is_reserved);
        EXPECT_FALSE(memory.get_region_info(view + page * 2).is_reserved);
        EXPECT_EQ(memory.shared_view_source(view), 0u);
        EXPECT_EQ(memory.shared_view_source(view + page * 2), 0u);
        EXPECT_TRUE(memory.get_region_info(source).is_reserved);
        token.reset();
        EXPECT_EQ(*destroyed, 1u);
    }

    TEST(HostMemoryLifetimeTest, SizedWholeViewReleaseRemovesRelationship)
    {
        lifetime_memory backend;
        memory_manager memory{backend};
        auto destroyed = std::make_shared<size_t>();
        auto storage = std::make_shared<host_storage>(destroyed);
        ASSERT_TRUE(memory.allocate_host_memory_at(source, page * 3, storage->bytes.data(), memory_permission::read_write, storage));
        ASSERT_TRUE(memory.allocate_shared_view(view, source, page * 3, memory_permission::read_write));
        ASSERT_TRUE(memory.release_memory(view, page * 3));
        EXPECT_EQ(memory.shared_view_source(view), 0u);
        EXPECT_FALSE(memory.has_shared_views(source));
        EXPECT_TRUE(memory.get_region_info(source).is_reserved);
    }

    TEST(HostMemoryLifetimeTest, HostMapExceptionReleasesReservationAndStorage)
    {
        lifetime_memory backend;
        memory_manager memory{backend};
        auto destroyed = std::make_shared<size_t>();
        auto storage = std::make_shared<host_storage>(destroyed);
        void* pointer = storage->bytes.data();
        backend.fail_host_map = true;
        EXPECT_THROW(memory.allocate_host_memory(page * 3, pointer, memory_permission::read_write, std::move(storage)), std::runtime_error);
        EXPECT_EQ(memory.compute_memory_stats().reserved_memory, 0u);
        EXPECT_TRUE(backend.pages.empty());
        EXPECT_EQ(backend.claims, 1u);
        EXPECT_GT(backend.claim_releases, 0u);
        EXPECT_EQ(*destroyed, 1u);
    }

    TEST(HostMemoryLifetimeTest, SharedMapExceptionRollsBackEarlierSegments)
    {
        lifetime_memory backend;
        memory_manager memory{backend};
        auto destroyed = std::make_shared<size_t>();
        auto storage = std::make_shared<host_storage>(destroyed);
        ASSERT_TRUE(memory.allocate_host_memory_at(source, page * 3, storage->bytes.data(), memory_permission::read_write, storage));
        ASSERT_TRUE(memory.protect_memory(source + page, page, memory_permission::read));
        backend.fail_shared_call = 2;
        EXPECT_THROW(memory.allocate_shared_view(view, source, page * 3, memory_permission::read_write), std::runtime_error);
        EXPECT_FALSE(memory.get_region_info(view).is_reserved);
        EXPECT_EQ(memory.shared_view_source(view), 0u);
        EXPECT_FALSE(backend.pages.contains(view));
        EXPECT_TRUE(backend.pages.contains(source));
        EXPECT_EQ(*destroyed, 0u);
    }

    TEST(HostMemoryLifetimeTest, FailedRevocationPreservesBackingAndReportsFailure)
    {
        lifetime_memory backend;
        memory_manager memory{backend};
        auto destroyed = std::make_shared<size_t>();
        auto storage = std::make_shared<host_storage>(destroyed);
        ASSERT_TRUE(memory.allocate_host_memory_at(source, page * 3, storage->bytes.data(), memory_permission::read_write, storage));
        auto token = memory.host_memory_backing_at(source);
        ASSERT_TRUE(memory.allocate_shared_view(view, source, page * 3, memory_permission::read_write));
        storage.reset();
        backend.fail_unmap = source;
        EXPECT_THROW(memory.revoke_host_memory(token), std::runtime_error);
        EXPECT_TRUE(memory.get_region_info(source).is_reserved);
        EXPECT_TRUE(memory.get_region_info(view).is_reserved);
        EXPECT_EQ(*destroyed, 0u);
        static_cast<memory_interface&>(memory).write_memory<uint64_t>(source, 42);
        EXPECT_EQ(memory.read_memory<uint64_t>(view), 42u);
        backend.fail_unmap.reset();
        EXPECT_NO_THROW(memory.revoke_host_memory(token));
        token.reset();
        EXPECT_EQ(*destroyed, 1u);
    }

    TEST(HostMemoryLifetimeTest, SizedReleaseRecordsPartialProgressBeforeLaterUnmapFailure)
    {
        lifetime_memory backend;
        memory_manager memory{backend};
        auto destroyed = std::make_shared<size_t>();
        auto storage = std::make_shared<host_storage>(destroyed);
        ASSERT_TRUE(memory.allocate_host_memory_at(source, page * 3, storage->bytes.data(), memory_permission::read_write, storage));
        ASSERT_TRUE(memory.protect_memory(source + page, page, memory_permission::read));
        ASSERT_TRUE(memory.allocate_shared_view(view, source, page * 3, memory_permission::read_write));
        auto token = memory.host_memory_backing_at(view);
        storage.reset();
#if SOGEN_REFLECTION_LEVEL > 0
        const auto old_version = memory.get_layout_version();
#endif
        backend.fail_unmap = view + page;
        EXPECT_THROW(memory.release_memory(view, page * 2), std::runtime_error);
        EXPECT_TRUE(memory.get_region_info(view).is_reserved);
        EXPECT_FALSE(memory.get_region_info(view).is_committed);
        EXPECT_TRUE(memory.get_region_info(view + page).is_committed);
        EXPECT_TRUE(memory.get_region_info(view + page * 2).is_committed);
        EXPECT_EQ(memory.host_memory_backing_at(view), token);
        EXPECT_EQ(memory.shared_view_source(view), source);
#if SOGEN_REFLECTION_LEVEL > 0
        EXPECT_GT(memory.get_layout_version(), old_version);
#endif
        EXPECT_EQ(*destroyed, 0u);
        static_cast<memory_interface&>(memory).write_memory<uint64_t>(source + page, 42);
        EXPECT_EQ(memory.read_memory<uint64_t>(view + page), 42u);
        EXPECT_EQ(backend.unmap_attempts, (std::vector<uint64_t>{view, view + page}));

        backend.fail_unmap.reset();
        ASSERT_TRUE(memory.release_memory(view, page * 2));
        EXPECT_EQ(backend.unmap_attempts, (std::vector<uint64_t>{view, view + page, view + page}));
        EXPECT_FALSE(memory.get_region_info(view).is_reserved);
        EXPECT_FALSE(memory.get_region_info(view + page).is_reserved);
        EXPECT_TRUE(memory.get_region_info(view + page * 2).is_committed);
        EXPECT_EQ(memory.shared_view_source(view), 0u);
        EXPECT_EQ(memory.shared_view_source(view + page * 2), source + page * 2);
        EXPECT_EQ(memory.host_memory_backing_at(view + page * 2), token);
        ASSERT_TRUE(memory.release_memory(source, 0));
        EXPECT_EQ(*destroyed, 0u);
        ASSERT_TRUE(memory.release_memory(view + page * 2, 0));
        EXPECT_EQ(*destroyed, 0u);
        token.reset();
        EXPECT_EQ(*destroyed, 1u);
    }

    TEST(HostMemoryLifetimeTest, RuntimeOwnershipDoesNotChangeSerializedRegionBytes)
    {
        lifetime_memory first_backend;
        lifetime_memory second_backend;
        memory_manager first{first_backend};
        memory_manager second{second_backend};
        auto destroyed = std::make_shared<size_t>();
        auto first_storage = std::make_shared<host_storage>(destroyed);
        auto second_storage = std::make_shared<host_storage>(destroyed);
        ASSERT_TRUE(
            first.allocate_host_memory_at(source, page * 3, first_storage->bytes.data(), memory_permission::read_write, first_storage));
        ASSERT_TRUE(
            second.allocate_host_memory_at(source, page * 3, second_storage->bytes.data(), memory_permission::read_write, second_storage));
        utils::buffer_serializer rejected;
        EXPECT_THROW(first.serialize_memory_state(rejected, false), std::runtime_error);
        first.host_memory_backing_at(source)->snapshot_reconstructible = true;
        second.host_memory_backing_at(source)->snapshot_reconstructible = true;
        utils::buffer_serializer left;
        utils::buffer_serializer right;
        first.serialize_memory_state(left, false);
        second.serialize_memory_state(right, false);
        EXPECT_EQ(left.get_buffer(), right.get_buffer());
        first.host_memory_backing_at(source)->snapshot_reconstructible = false;
        utils::buffer_serializer retired;
        EXPECT_THROW(first.serialize_memory_state(retired, false), std::runtime_error);
    }
}
