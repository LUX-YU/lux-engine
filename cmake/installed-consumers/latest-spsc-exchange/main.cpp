#include <lux/engine/scene/LatestSpscExchange.hpp>
int main()
{
    lux::scene::LatestSpscExchange<int> exchange;
    exchange.write() = 7;
    exchange.publish();
    return exchange.acquireLatest() && exchange.read() == 7 ? 0 : 1;
}
