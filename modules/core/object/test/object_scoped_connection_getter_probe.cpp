#include <lux/engine/object/Connection.hpp>

int
main()
{
    lux::object::ScopedConnection connection;
    static_cast<void>(connection.connection());
}
