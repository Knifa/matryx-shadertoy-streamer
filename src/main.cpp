#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include <signal.h>

#include <argparse/argparse.hpp>
#include <jpeglib.h>
#include <libwebsockets.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

class Args {
public:
  int width;
  int height;
  std::string outputEndpoint;

  Args(int argc, char *argv[]) {
    argparse::ArgumentParser program("matryx_shadertoy_streamer");

    program.add_argument("--width").default_value(192).scan<'i', int>();
    program.add_argument("--height").default_value(320).scan<'i', int>();
    program.add_argument("--output-endpoint")
        .default_value(std::string("ipc:///tmp/matryx-shadertoy-output.sock"));

    try {
      program.parse_args(argc, argv);
    } catch (const std::runtime_error &err) {
      std::cout << err.what() << std::endl;
      std::cout << program;
      throw;
    }

    width = program.get<int>("--width");
    height = program.get<int>("--height");
    outputEndpoint = program.get<std::string>("--output-endpoint");
  }

  void print() {
    std::cout << "width: " << width << std::endl;
    std::cout << "height: " << height << std::endl;
    std::cout << "outputEndpoint: " << outputEndpoint << std::endl;
  }
};

Args *args;

std::mutex latestPixelsMutex;
void *latestPixels = nullptr;
unsigned int latestPixelsSize = 0;
unsigned int latestPixelsIndex = 0;
unsigned int stopping = 0;

struct matryxPerSessionData {
  unsigned char *pixels;
  unsigned int pixelsSize;
  unsigned int lastSentIndex;
};

struct matryxSharedData {
  std::mutex mutex;
  unsigned int activeConnections;
};

struct lws_context *lwsContext;
struct matryxSharedData matryxSharedData;

static int matryxCallback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                          size_t len) {
  struct matryxPerSessionData *pss = (struct matryxPerSessionData *)user;

  switch (reason) {
  case LWS_CALLBACK_ESTABLISHED:
    std::cout << "LWS_CALLBACK_ESTABLISHED" << std::endl;

    pss->pixelsSize = (args->width * args->height * 3); // Roughly.
    pss->pixelsSize = LWS_PRE + pss->pixelsSize;
    pss->pixels = new unsigned char[pss->pixelsSize];

    matryxSharedData.mutex.lock();
    matryxSharedData.activeConnections += 1;
    matryxSharedData.mutex.unlock();

    break;

  case LWS_CALLBACK_SERVER_WRITEABLE: {
    // std::cout << "LWS_CALLBACK_SERVER_WRITEABLE" << std::endl;

    latestPixelsMutex.lock();
    if (pss->lastSentIndex == latestPixelsIndex) {
      latestPixelsMutex.unlock();
      break;
    }

    int pixelsSize = latestPixelsSize;
    pss->lastSentIndex = latestPixelsIndex;

    if (pixelsSize > pss->pixelsSize) {
      std::cerr << "pixelsSize > pss->pixelsSize" << std::endl;
      latestPixelsMutex.unlock();
      break;
    }

    std::memcpy(pss->pixels + LWS_PRE, latestPixels, pixelsSize);
    latestPixelsMutex.unlock();

    int result = lws_write(wsi, pss->pixels + LWS_PRE, pixelsSize, LWS_WRITE_BINARY);
    if (result < 0) {
      std::cerr << "lws_write failed" << std::endl;
    }

    break;
  }

  case LWS_CALLBACK_CLOSED:
    std::cout << "LWS_CALLBACK_CLOSED" << std::endl;
    delete[] pss->pixels;

    matryxSharedData.mutex.lock();
    matryxSharedData.activeConnections -= 1;
    matryxSharedData.mutex.unlock();

    break;

  default:
    break;
  }

  return 0;
}

const struct lws_protocols protocols[] = {
    {"matryx", matryxCallback, sizeof(struct matryxPerSessionData), 0},
    {NULL, NULL, 0, 0},
};

void zmq_thread() {
  std::cout << "Starting ZMQ thread" << std::endl;

  zmq::context_t context;
  zmq::socket_t socket(context, ZMQ_SUB);
  socket.connect(args->outputEndpoint);
  socket.setsockopt(ZMQ_SUBSCRIBE, "output", 6);

  std::chrono::time_point<std::chrono::steady_clock> nextFrameTime =
      std::chrono::steady_clock::now();

  while (!stopping) {
    zmq::multipart_t message;
    message.recv(socket);

    const auto now = std::chrono::steady_clock::now();
    if (now >= nextFrameTime) {
      nextFrameTime = now + std::chrono::milliseconds(1000 / 30);
    } else {
      continue;
    }

    matryxSharedData.mutex.lock();
    if (matryxSharedData.activeConnections == 0) {
      matryxSharedData.mutex.unlock();
      continue;
    }
    matryxSharedData.mutex.unlock();

    message.pop();

    const auto pixelMessage = message.pop();
    const unsigned char *pixels = pixelMessage.data<unsigned char>();

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char *jpegOut = nullptr;
    unsigned long jpegOutSize = 0;
    jpeg_mem_dest(&cinfo, &jpegOut, &jpegOutSize);

    cinfo.image_width = args->width;
    cinfo.image_height = args->height;
    cinfo.input_components = 4;
    cinfo.in_color_space = JCS_EXT_RGBA;
    jpeg_set_defaults(&cinfo);

    cinfo.dct_method = JDCT_IFAST;
    for (int i = 0; i < cinfo.num_components; i++) {
      cinfo.comp_info[i].h_samp_factor = 1;
      cinfo.comp_info[i].v_samp_factor = 1;
    }

    jpeg_set_quality(&cinfo, 95, true);
    jpeg_start_compress(&cinfo, false);

    JSAMPROW row_pointer[args->height];
    for (int i = 0; i < args->height; i++) {
      row_pointer[i] = (JSAMPROW)&pixels[i * args->width * 4];
    }

    jpeg_write_scanlines(&cinfo, row_pointer, args->height);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    latestPixelsMutex.lock();
    if (latestPixels != nullptr) {
      std::free(latestPixels);
    }

    latestPixels = jpegOut;
    latestPixelsSize = jpegOutSize;
    latestPixelsIndex += 1;
    latestPixelsMutex.unlock();

    lws_callback_on_writable_all_protocol(lwsContext, &protocols[0]);
    lws_cancel_service(lwsContext);
  }

  std::cout << "zmq_thread exiting" << std::endl;
}

int main(int argc, char *argv[]) {
  args = new Args(argc, argv);
  args->print();

  lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, nullptr);

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));

  const lws_retry_bo_t retry = {
      .secs_since_valid_ping = 3,
      .secs_since_valid_hangup = 10,
  };

  info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
  info.pcontext = &lwsContext;
  info.port = 42025;
  info.protocols = protocols;
  info.retry_and_idle_policy = &retry;
  info.vhost_name = "localhost";
  info.ws_ping_pong_interval = 10, lwsContext = lws_create_context(&info);

  std::thread zmqThreadHandle(zmq_thread);

  std::cout << "Starting LWS" << std::endl;
  while (!lws_service(lwsContext, 0)) {
    // Doin' it.
  }

  stopping = 1;
  zmqThreadHandle.join();

  lws_context_destroy(lwsContext);

  return 0;
}
