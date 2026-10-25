#include "business_data.h"
#include "redis_client.h"
#include "log.h"
#include <QDebug>

// ═══════════════════════════════════════════════════════════════
// 种子数据：电商秒杀场景
// 数据写入是幂等的 —— 只有 product:1001 不存在时才写入
// ═══════════════════════════════════════════════════════════════

void BusinessData::initIfEmpty(RedisClient* client,
                               std::function<void()> onDone) {
    if (!client || !client->isConnected()) {
        if (onDone) onDone();
        return;
    }

    // 检查哨兵 key 是否存在（幂等保护）
    client->sendCommand(
        QStringList() << "EXISTS" << "product:1001",
        [client, onDone](const RespValue& v) {
            if (v.type == RespValue::INTEGER && v.int_val == 1) {
                Log::info("Business data already exists, skipping seed");
                if (onDone) onDone();
            } else {
                Log::info("Seeding business data...");
                seedAll(client, onDone);
            }
        });
}

void BusinessData::seedAll(RedisClient* client,
                           std::function<void()> onDone) {

    // ── 商品数据（8 件商品，覆盖 5 个品类）──────────────────
    // product:{id} → Hash

    // 手机数码
    client->sendCommand(QStringList()
        << "HSET" << "product:1001"
        << "name"    << "iPhone 15 Pro Max 256GB"
        << "price"   << "8999"
        << "stock"   << "200"
        << "sold"    << "0"
        << "category" << "手机数码"
        << "brand"   << "Apple");

    client->sendCommand(QStringList()
        << "HSET" << "product:1002"
        << "name"    << "华为 Mate 60 Pro 512GB"
        << "price"   << "6999"
        << "stock"   << "150"
        << "sold"    << "0"
        << "category" << "手机数码"
        << "brand"   << "Huawei");

    client->sendCommand(QStringList()
        << "HSET" << "product:1003"
        << "name"    << "Xiaomi 14 Ultra"
        << "price"   << "5999"
        << "stock"   << "300"
        << "sold"    << "0"
        << "category" << "手机数码"
        << "brand"   << "Xiaomi");

    // 电脑办公
    client->sendCommand(QStringList()
        << "HSET" << "product:1004"
        << "name"    << "MacBook Pro 14\" M3 Pro"
        << "price"   << "14999"
        << "stock"   << "80"
        << "sold"    << "0"
        << "category" << "电脑办公"
        << "brand"   << "Apple");

    client->sendCommand(QStringList()
        << "HSET" << "product:1005"
        << "name"    << "ThinkPad X1 Carbon Gen 12"
        << "price"   << "10999"
        << "stock"   << "60"
        << "sold"    << "0"
        << "category" << "电脑办公"
        << "brand"   << "Lenovo");

    // 耳机音箱
    client->sendCommand(QStringList()
        << "HSET" << "product:1006"
        << "name"    << "Sony WH-1000XM5 头戴式降噪耳机"
        << "price"   << "2499"
        << "stock"   << "500"
        << "sold"    << "0"
        << "category" << "耳机音箱"
        << "brand"   << "Sony");

    client->sendCommand(QStringList()
        << "HSET" << "product:1007"
        << "name"    << "AirPods Pro 2 (USB-C)"
        << "price"   << "1899"
        << "stock"   << "400"
        << "sold"    << "0"
        << "category" << "耳机音箱"
        << "brand"   << "Apple");

    // 智能穿戴
    client->sendCommand(QStringList()
        << "HSET" << "product:1008"
        << "name"    << "Apple Watch Ultra 2"
        << "price"   << "6499"
        << "stock"   << "90"
        << "sold"    << "0"
        << "category" << "智能穿戴"
        << "brand"   << "Apple");

    // ── 秒杀活动配置 ──────────────────────────────────────
    // flash:sale:{id} → Hash（活动元信息）
    client->sendCommand(QStringList()
        << "HSET" << "flash:sale:1"
        << "title"          << "618 数码秒杀专场"
        << "product_ids"    << "1001,1002,1003,1004,1005,1006,1007,1008"
        << "discount_pct"   << "50"
        << "start_time"     << "1718697600"
        << "end_time"       << "1718784000"
        << "limit_per_user" << "1"
        << "status"         << "upcoming");

    // 每件秒杀商品的独立库存和秒杀价
    client->sendCommand(QStringList()
        << "HSET" << "flash:1:product:1001"
        << "flash_price" << "4499"
        << "flash_stock" << "50"
        << "sold"        << "0");

    client->sendCommand(QStringList()
        << "HSET" << "flash:1:product:1003"
        << "flash_price" << "2999"
        << "flash_stock" << "80"
        << "sold"        << "0");

    client->sendCommand(QStringList()
        << "HSET" << "flash:1:product:1006"
        << "flash_price" << "1249"
        << "flash_stock" << "100"
        << "sold"        << "0");

    client->sendCommand(QStringList()
        << "HSET" << "flash:1:product:1002"
        << "flash_price" << "3499"
        << "flash_stock" << "40"
        << "sold"        << "0");

    client->sendCommand(QStringList()
        << "HSET" << "flash:1:product:1004"
        << "flash_price" << "7499"
        << "flash_stock" << "20"
        << "sold"        << "0");

    client->sendCommand(QStringList()
        << "HSET" << "flash:1:product:1008"
        << "flash_price" << "3249"
        << "flash_stock" << "30"
        << "sold"        << "0");

    client->sendCommand(QStringList()
        << "HSET" << "flash:1:product:1005"
        << "flash_price" << "5499"
        << "flash_stock" << "30"
        << "sold"        << "0");

    client->sendCommand(QStringList()
        << "HSET" << "flash:1:product:1007"
        << "flash_price" << "949"
        << "flash_stock" << "150"
        << "sold"        << "0");

    // ── 秒杀库存计数器（String: 支持原子 DECR 防超卖）────
    // DECR 对不存在的 key 返回 -1，所以必须提前初始化
    client->sendCommand(QStringList()
        << "SET" << "flash:1:product:1001:stock" << "50");
    client->sendCommand(QStringList()
        << "SET" << "flash:1:product:1002:stock" << "40");
    client->sendCommand(QStringList()
        << "SET" << "flash:1:product:1003:stock" << "80");
    client->sendCommand(QStringList()
        << "SET" << "flash:1:product:1004:stock" << "20");
    client->sendCommand(QStringList()
        << "SET" << "flash:1:product:1006:stock" << "100");
    client->sendCommand(QStringList()
        << "SET" << "flash:1:product:1008:stock" << "30");
    client->sendCommand(QStringList()
        << "SET" << "flash:1:product:1005:stock" << "30");
    client->sendCommand(QStringList()
        << "SET" << "flash:1:product:1007:stock" << "150");

    // ── 订单队列（空队列，等待模拟下单填充）──────────────
    client->sendCommand(QStringList()
        << "LPUSH" << "orders:pending" << "__placeholder__");
    client->sendCommand(QStringList()
        << "LPOP" << "orders:pending");  // 清理占位符，保持空队列

    // ── 运营统计计数器 ──────────────────────────────────
    client->sendCommand(QStringList()
        << "SET" << "stats:daily:orders" << "0");
    client->sendCommand(QStringList()
        << "SET" << "stats:daily:revenue" << "0");
    client->sendCommand(QStringList()
        << "SET" << "stats:daily:visitors" << "0");

    // ── 哨兵命令：所有写命令完成后回调 onDone ──────────
    // 利用 RESP 协议的 FIFO 回调队列，PING 的响应到达时
    // 前面所有写命令已经处理完毕
    client->sendCommand(
        QStringList() << "PING",
        [onDone](const RespValue&) {
            Log::info("Business data seeding complete");
            if (onDone) onDone();
        });
}
