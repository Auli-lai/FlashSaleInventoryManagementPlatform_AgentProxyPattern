#ifndef BUSINESS_DATA_H
#define BUSINESS_DATA_H

#include <functional>

class RedisClient;

// 业务数据初始化器 —— 将空的 Redis 实例变成一个"电商秒杀系统"
// 每次连接后自动检测：如果 product:1001 不存在，则写入全套种子数据
class BusinessData {
public:
    // 检测并写入种子数据，完成后回调 onDone
    // 幂等：如果数据已存在则跳过写入，直接回调
    static void initIfEmpty(RedisClient* client,
                            std::function<void()> onDone = nullptr);

private:
    static void seedAll(RedisClient* client,
                        std::function<void()> onDone);
};

#endif // BUSINESS_DATA_H
