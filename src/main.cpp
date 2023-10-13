#include <iostream>
#include <thread>
#include <vector>

#include <libwebsockets.h>

std::vector<struct lws *> clients;

const char *hello = "Hello, world!";

static int callback_matryx(struct lws *wsi, enum lws_callback_reasons reasons, void *user, void *in,
                           size_t len) {
  switch (reasons) {
  case LWS_CALLBACK_ESTABLISHED:
    std::cout << "LWS_CALLBACK_ESTABLISHED" << std::endl;
    clients.push_back(wsi);
    break;

  case LWS_CALLBACK_SERVER_WRITEABLE: {
    std::cout << "LWS_CALLBACK_SERVER_WRITEABLE" << std::endl;

    unsigned char *buf = (unsigned char *)malloc(LWS_PRE + strlen(hello) * 6400 + 1);
    for (int i = 0; i < 6400; i++) {
      memcpy(buf + LWS_PRE + strlen(hello) * i, hello, strlen(hello));
    }

    lws_write(wsi, buf + LWS_PRE, strlen(hello) * 6400, LWS_WRITE_TEXT);
    free(buf);
    break;
  }

  case LWS_CALLBACK_RECEIVE:
    std::cout << "LWS_CALLBACK_RECEIVE" << std::endl;
    std::cout << "Received: " << std::string((char *)in, len) << std::endl;
    lws_callback_on_writable(wsi);
    break;

  case LWS_CALLBACK_CLOSED:
    std::cout << "LWS_CALLBACK_CLOSED" << std::endl;
    clients.erase(std::remove(clients.begin(), clients.end(), wsi), clients.end());
    break;

  default:
    break;
  }

  return 0;
}

const struct lws_protocols protocols[] = {
    {"matryx", callback_matryx, 0, 0},
    {NULL, NULL, 0, 0},
};

void yeet_thread() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "yeet" << std::endl;

    for (auto client : clients) {
      lws_callback_on_writable(client);
    }
  }
}

int main(int argc, char *argv[]) {
  lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO, nullptr);

  std::thread yeet(yeet_thread);

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));

  info.port = 8080;
  info.protocols = protocols;
  info.pt_serv_buf_size = 32 * 1024;

  struct lws_context *context = lws_create_context(&info);
  if (context == NULL) {
    std::cout << "Failed to create libwebsocket context" << std::endl;
    return -1;
  }

  int n = 0;
  while (n >= 0) {
    n = lws_service(context, 0);
    std::cout << "n: " << n << std::endl;
  }

  lws_context_destroy(context);

  return 0;
}
