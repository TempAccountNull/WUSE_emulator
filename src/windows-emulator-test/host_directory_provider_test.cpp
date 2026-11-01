#include <gtest/gtest.h>

#include <filesystem/host_directory_provider.hpp>

#include <fstream>
#include <random>

namespace sogen::test
{
    TEST(HostDirectoryProviderTest, KeepsThousandsOfFilesOpen)
    {
        const auto root = std::filesystem::temp_directory_path() /
                          ("sogen-host-provider-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(root);
        std::ofstream(root / "package.pkg", std::ios::binary) << "data";

        {
            host_directory_provider provider{root};
            const windows_path file{std::nullopt, std::list<std::u16string>{u"package.pkg"}};

            std::vector<std::unique_ptr<i_file>> handles{};
            for (size_t i = 0; i < 2000; ++i)
            {
                auto result = provider.open(file, u"rb");
                ASSERT_EQ(result.status, STATUS_SUCCESS) << "open " << i;
                handles.push_back(std::move(result.file));
            }
        }

        std::error_code ec{};
        std::filesystem::remove_all(root, ec);
    }
}
