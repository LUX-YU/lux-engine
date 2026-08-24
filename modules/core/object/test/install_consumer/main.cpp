#include "InstalledObject.hpp"

int main()
{
    InstalledObject object;
    int observed = 0;
    auto connection = object.observeScoped<InstalledObject::changed>(
        [&observed](const int &value) noexcept { observed = value; });
    object.publish(17);
    return connection.connected() && observed == 17 ? 0 : 1;
}
