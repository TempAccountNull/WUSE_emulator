#pragma once

#include "../port.hpp"

namespace sogen
{
    std::unique_ptr<port> create_sspi_service_port();
}
