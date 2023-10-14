#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include <libwebsockets.h>
#include <png.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

std::list<struct lws *> clients;

std::mutex latestPixelsMutex;
void *latestPixels = nullptr;
int latestPixelsSize = 0;

static int callback_matryx(struct lws *wsi, enum lws_callback_reasons reasons, void *user, void *in,
                           size_t len) {
  switch (reasons) {
  case LWS_CALLBACK_ESTABLISHED:
    std::cout << "LWS_CALLBACK_ESTABLISHED" << std::endl;
    clients.push_back(wsi);
    break;

  case LWS_CALLBACK_SERVER_WRITEABLE: {
    // std::cout << "LWS_CALLBACK_SERVER_WRITEABLE" << std::endl;

    latestPixelsMutex.lock();
    int pixelsSize = latestPixelsSize;
    int bufSize = LWS_PRE + pixelsSize;
    unsigned char *buf = (unsigned char *)std::malloc(bufSize);
    std::memcpy(buf + LWS_PRE, latestPixels, pixelsSize);
    latestPixelsMutex.unlock();

    lws_write(wsi, buf + LWS_PRE, pixelsSize, LWS_WRITE_BINARY);
    std::free(buf);

    break;
  }

  case LWS_CALLBACK_RECEIVE:
    std::cout << "LWS_CALLBACK_RECEIVE" << std::endl;
    break;

  case LWS_CALLBACK_CLOSED:
    std::cout << "LWS_CALLBACK_CLOSED" << std::endl;
    clients.remove(wsi);
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

    for (auto client : clients) {
      lws_callback_on_writable(client);
    }
  }
}

void zmq_thread() {
  zmq::context_t context;
  zmq::socket_t socket(context, ZMQ_SUB);

  png_image image;
  memset(&image, 0, sizeof(image));

  image.version = PNG_IMAGE_VERSION;
  image.width = 192;
  image.height = 320;
  image.format = PNG_FORMAT_RGBA;
  image.flags = PNG_IMAGE_FLAG_FAST;

  socket.connect("tcp://127.0.0.1:42025");
  socket.set(zmq::sockopt::subscribe, "layers");

  while (true) {
    zmq::multipart_t message;
    message.recv(socket);

    // Pop topic.
    message.pop();

    const auto count = message.poptyp<int>();

    const auto pixelMessage = message.pop();
    const unsigned char *pixels = pixelMessage.data<unsigned char>();

    void *pngPixels = std::malloc(PNG_IMAGE_DATA_SIZE(image));

    png_alloc_size_t pngPixelsSize = PNG_IMAGE_DATA_SIZE(image);
    png_image_write_to_memory(&image, pngPixels, &pngPixelsSize, 0,
                              pixels + (192 * 320 * 4 * (count - 1)), 0, nullptr);

    latestPixelsMutex.lock();
    if (latestPixels != nullptr) {
      std::free(latestPixels);
    }

    latestPixels = pngPixels;
    latestPixelsSize = pngPixelsSize;
    latestPixelsMutex.unlock();

    for (auto client : clients) {
      lws_callback_on_writable(client);
    }
  }
}

int main(int argc, char *argv[]) {
  // lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO, nullptr);

  std::thread yeetThreadHandle(yeet_thread);
  std::thread zmqThreadHandle(zmq_thread);

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));

  info.port = 8080;
  info.protocols = protocols;

  struct lws_context *context = lws_create_context(&info);
  if (context == NULL) {
    std::cout << "Failed to create libwebsocket context" << std::endl;
    return -1;
  }

  int n = 0;
  while (n >= 0) {
    n = lws_service(context, 0);
  }

  lws_context_destroy(context);

  yeetThreadHandle.join();
  zmqThreadHandle.join();

  return 0;
}
