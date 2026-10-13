#include "../Support.hpp"

int main()
{
    using namespace transport_test;
    RenderRouteTable table(1);
    auto descriptor = operationDescriptor<Value>("installed.noop");
    must(table.registerOperation(descriptor, nullptr, voidHandler));
    auto receiver = must(RenderTransport::create(std::move(table)));
    auto sender = receiver->sender();
    auto bound = must(sender.bind<Value>(descriptor));
    auto packet = sender.makeBatch();
    auto requests = sender.makeRequests();
    must(packet.append(bound, encode(1)));
    must(sender.submit(packet, requests, EDelivery::COMMAND));
    must(receiver->poll(EDelivery::COMMAND));
    check(terminal(requests.requests()[0]).outcome == EOperationOutcome::SUCCESS, "installed void terminal");
    receiver->requestStop();
    check(receiver->drained(), "installed drain");
}
