std::shared_ptr<RedisValue> publish(std::string channel, std::string message);
std::shared_ptr<RedisValue> subscribe(const std::vector<std::string> &channels, std::function<void(const std::vector<SubMessage> &messages)> msg_callback);
std::shared_ptr<RedisValue> psubscribe(const std::vector<std::string> &patterns, std::function<void(const std::vector<PSubMessage> &messages)> pmsg_callback);
std::shared_ptr<RedisValue> unsubscribe(const std::vector<std::string> &channels);
std::shared_ptr<RedisValue> punsubscribe(const std::vector<std::string> &patterns);
std::shared_ptr<RedisValue> subscribe_mixed(const std::vector<std::string> &channels, const std::vector<std::string> &patterns);
std::shared_ptr<RedisValue> unsubscribe_mixed(const std::vector<std::string> &channels, const std::vector<std::string> &patterns);
std::shared_ptr<RedisValue> update_subscriptions(
    const std::vector<std::string> &subscribe_channels,
    const std::vector<std::string> &subscribe_patterns,
    const std::vector<std::string> &unsubscribe_channels,
    const std::vector<std::string> &unsubscribe_patterns);
