#include "buff/buffer.h"
#include "net/http/request.h"
#include <iostream>
#include <string>
#include "nlohmann/json_fwd.hpp"
#include "nlohmann/json.hpp"


using namespace std;

int main()
{
    string raw = "GET /Michel4Liu/article/details/79531484 HTTP/1.1\r\n"
                "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n"
                "Accept-Encoding: gzip, deflate, br\r\n"
                "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8,en-GB;q=0.7,en-US;q=0.6,ja;q=0.5\r\n"
                "Cache-Control: max-age=0\r\n"
                "Connection: keep-alive\r\n"
                "Cookie: BAIDU_SSP_lcr=https://cn.bing.com/; uuid_tt_dd=10_19001793670-1694633276924-244360; UserName=qq_38214064; UserInfo=f8a19b802ec945c68e80b86d51e46c94; UserToken=f8a19b802ec945c68e80b86d51e46c94; UserNick=1997HelloWorld; AU=9E6; UN=qq_38214064; BT=1697109839848; p_uid=U010000; Hm_up_6bcd52f51e9b3dce32bec4a3997715ac=%7B%22islogin%22%3A%7B%22value%22%3A%221%22%2C%22scope%22%3A1%7D%2C%22isonline%22%3A%7B%22value%22%3A%221%22%2C%22scope%22%3A1%7D%2C%22isvip%22%3A%7B%22value%22%3A%220%22%2C%22scope%22%3A1%7D%2C%22uid_%22%3A%7B%22value%22%3A%22qq_38214064%22%2C%22scope%22%3A1%7D%7D; FCNEC=%5B%5B%22AKsRol_dIVf4-VQOY7CiFm34ECeBp1GozYTEzQhNu6ffblRDN2Qmts-21bKguzoZmCSEMwVhvAWpVieYcHPqwCGnWicmHg4C74ICHSYfhAXGZFOgwzJOzhK6MIoKC9eAcQ_sdhSSzhG33fGdXLO10Niow2M_R8nUYg%3D%3D%22%5D%2Cnull%2C%5B%5D%5D; qq_38214064comment_new=1668302555115; c_pref=; c_ref=https%3A//cn.bing.com/; c_first_ref=cn.bing.com; c_segment=8; firstDie=1; Hm_lvt_6bcd52f51e9b3dce32bec4a3997715ac=1700397786,1700666320,1700750164,1700982523; dc_sid=50462ddeca8d7b1bcd832d32f785ff84; creative_btn_mp=3; log_Id_click=194; dc_session_id=10_1701003122663.383229; SidecHatdocDescBoxNum=true; c_first_page=https%3A//blog.csdn.net/Michel4Liu/article/details/79531484; c_dsid=11_1701003606400.233306; c_page_id=default; log_Id_pv=197; Hm_lpvt_6bcd52f51e9b3dce32bec4a3997715ac=1701003607; __gads=ID=a6bc64bbcb8cc2bf-225e355da0e300c7:T=1694633278:RT=1701003607:S=ALNI_MZbnafmwW1OKSyCTUvApRUbzlROWQ; __gpi=UID=00000d926eba86f6:T=1694633278:RT=1701003607:S=ALNI_MZEm9oG8xzsvLNJGJkdu7tdBy1NRw; log_Id_view=5691; dc_tos=s4qfyh\r\n"
                "Host: blog.csdn.net\r\n"
                "Referer: https://cn.bing.com/\r\n"
                "Sec-Fetch-Dest: document\r\n"
                "Sec-Fetch-Mode: navigate\r\n"
                "Sec-Fetch-Site: cross-site\r\n"
                "Sec-Fetch-User: ?1\r\n"
                "Upgrade-Insecure-Requests: 1\r\n"
                "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36 Edg/119.0.0.0\r\n"
                "sec-ch-ua: \"Microsoft Edge\";v=\"119\", \"Chromium\";v=\"119\", \"Not?A_Brand\";v=\"24\"\r\n"
                "sec-ch-ua-mobile: ?0\r\n"
                "sec-ch-ua-platform: \"Windows\"\r\n\r\n";

    HttpRequest req;
    Buffer buff;
    buff.write_string(raw);
    if (!req.parse_header(buff)) {
        std::cout << "parse fail!!" << std::endl;
        return 1;
    }

    using namespace nlohmann;
    json data = {
        {"name", "tomcat"}
    };

    cout << data << endl;

    return 0;
}