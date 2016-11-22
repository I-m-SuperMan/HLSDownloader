#ifndef __HLS_DownLoad__curl__
#define __HLS_DownLoad__curl__
#include <string>
#define STRING 0x0001
#define BINKEY 0x0002
#define BINARY 0x0003
#define strdup(A) _strdup(A)

#define USER_AGENT "Mozilla/5.0 (iPad; CPU OS 6_0 like Mac OS X) " \
                   "AppleWebKit/536.26 (KHTML, like Gecko) Version/6.0 " \
                   "Mobile/10A5355d Safari/8536.25"


size_t get_data_from_url(const char *url, char **str, uint8_t **bin, int type,std::string user_agent=USER_AGENT);

#endif /* defined(__HLS_DownLoad__curl__) */
