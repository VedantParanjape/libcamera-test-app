#include <iomanip>
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <sys/mman.h>

#include <libcamera/libcamera.h>

#include <QApplication>
#include <QLabel>

using namespace libcamera;

std::shared_ptr<libcamera::Camera> camera;
QImage myImage;
QLabel *myLabel = NULL;

// void libcamera_close(std::shared_ptr<libcamera::Camera> camera, FrameBufferAllocator *allocator, Stream* stream, libcamera::CameraManager *cm)
// {
//     std::cout << "end\n";
//     camera->stop();
//     allocator->free(stream);
//     delete allocator;
//     camera->release();
//     camera.reset();
//     cm->stop();
// }

static const QMap<libcamera::PixelFormat, QImage::Format> nativeFormats
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 2, 0)
    {libcamera::formats::ABGR8888, QImage::Format_RGBA8888},
#endif
        {libcamera::formats::ARGB8888, QImage::Format_RGB32},
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        {libcamera::formats::RGB888, QImage::Format_BGR888},
#endif
        {libcamera::formats::BGR888, QImage::Format_RGB888},
};

static void requestComplete(Request *request)
{
    if (request->status() == Request::RequestCancelled)
    {
        return;
    }

    const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

    for (auto bufferPair : buffers)
    {
        FrameBuffer *buffer = bufferPair.second;

        const FrameMetadata &metadata = buffer->metadata();
        std::cout << " seq: " << std::setw(6) << std::setfill('0') << metadata.sequence << " bytesused: ";

        unsigned int nplane = 0;
        for (const FrameMetadata::Plane &plane : metadata.planes)
        {
            std::cout << plane.bytesused;
            if (++nplane < metadata.planes.size())
                std::cout << "/";
        }

        std::cout << std::endl;

        size_t size = buffer->metadata().planes[0].bytesused;
        const FrameBuffer::Plane &plane = buffer->planes().front();
        void *memory = mmap(NULL, plane.length, PROT_READ, MAP_SHARED, plane.fd.fd(), 0);

        myImage.loadFromData(static_cast<unsigned char *>(memory), (int)size);
        myLabel->setPixmap(QPixmap::fromImage(myImage));
        myLabel->show();
    }

    request->reuse(Request::ReuseBuffers);

    if (!request)
    {
        std::cerr << "Can't create request" << std::endl;
        return;
    }

    camera->queueRequest(request);
}

int main(int argc, char *argv[])
{
    QApplication window(argc, argv);

    libcamera::CameraManager *cm = new libcamera::CameraManager();
    cm->start();

    for (auto const &camera : cm->cameras())
    {
        std::cout << "Camera ID: " << camera->id() << std::endl;
    }

    std::string camera_id = cm->cameras()[0]->id();
    camera = cm->get(camera_id);
    camera->acquire();

    std::unique_ptr<libcamera::CameraConfiguration> config = camera->generateConfiguration({libcamera::StreamRole::Viewfinder});

    libcamera::StreamConfiguration &streamConfig = config->at(0);
    // streamConfig.size.width = 640;
    // streamConfig.size.height = 480;

    /* Use a format supported by the viewfinder if available. */
    std::vector<PixelFormat> formats = streamConfig.formats().pixelformats();
    for (const PixelFormat &format : nativeFormats.keys())
    {
        auto match = std::find_if(formats.begin(), formats.end(),
                                  [&](const PixelFormat &f) {
                                      return f == format;
                                  });
        if (match != formats.end())
        {
            streamConfig.pixelFormat = format;
            break;
        }
    }

    config->validate();

    std::cout << "Default viewfinder configuration is: " << streamConfig.toString() << std::endl;

    camera->configure(config.get());

    FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

    for (StreamConfiguration &cfg : *config)
    {
        int ret = allocator->allocate(cfg.stream());
        if (ret < 0)
        {
            std::cerr << "Can't allocate buffers" << std::endl;
            return -ENOMEM;
        }
    }

    Stream *stream = streamConfig.stream();
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
    std::vector<std::shared_ptr<Request>> requests;

    for (unsigned int i = 0; i < buffers.size(); ++i)
    {
        std::shared_ptr<Request> request = camera->createRequest();
        if (!request)
        {
            std::cerr << "Can't create request" << std::endl;
            return -ENOMEM;
        }

        const std::unique_ptr<FrameBuffer> &buffer = buffers[i];
        int ret = request->addBuffer(stream, buffer.get());
        if (ret < 0)
        {
            std::cerr << "Can't set buffer for request"
                      << std::endl;
            return ret;
        }

        requests.push_back(request);
    }

    camera->requestCompleted.connect(requestComplete);
    myLabel = new QLabel;

    camera->start();
    for (std::shared_ptr<libcamera::Request> request : requests)
    {
        camera->queueRequest(request.get());
    }

    int ret = window.exec();

    camera->stop();
    allocator->free(stream);
    delete allocator;
    camera->release();
    camera.reset();
    cm->stop();

    return ret;
}
