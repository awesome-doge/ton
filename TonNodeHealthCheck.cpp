#include <iostream>
#include "ton_node_api.h" // hipotetyczne API dla węzła TON

int main() {
    try {
        TonNode node;
        if (!node.connect("127.0.0.1", 3030)) {
            std::cerr << "❌ Connection to TON node failed" << std::endl;
            return 1;
        }

        auto status = node.getStatus();
        std::cout << "✅ Node status: " << status << std::endl;

        auto blockInfo = node.getMasterchainInfo();
        std::cout << "Masterchain latest block: " << blockInfo.seqno
                  << " hash: " << blockInfo.hash << std::endl;

    } catch (const std::exception &e) {
        std::cerr << "❌ Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
