#include "yuan/mysql/mysql_client.h"

#include "mysql_smoke_options.h"

#include <exception>
#include <iostream>

int main(int argc, char **argv)
{
    try {
        yuan::mysql::ConnectionOptions options = yuan::mysql::test::parse_connection_options(argc, argv);

        yuan::mysql::Connection connection(options);
        connection.ping();
        connection.execute("SET @yuan_mysql_smoke = ?", {yuan::mysql::string_value("mysql-client")});

        const auto result = connection.query("SELECT @yuan_mysql_smoke");
        if (result.row_count() != 1 || result.column_count() != 1 || !result.rows()[0][0] ||
            *result.rows()[0][0] != "mysql-client") {
            std::cerr << "unexpected smoke test result\n";
            return 1;
        }

        std::cout << "YuanMysql smoke test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
