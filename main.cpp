#include <iomanip>
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <ctime>
#include <sys/mman.h>

#include <libcamera/libcamera.h>

#include <QApplication>
#include <QLabel>
#include <QPainter>

using namespace libcamera;

// Pointer to camera object which is connected to a physical camera
static std::shared_ptr<libcamera::Camera> camera;
// QImage object which is used to display streamed images
static QImage viewfinder;
// Pointer to QLabel which shows 
static QLabel *viewfinder_label = NULL;
// Variable used to store time at which previous frame was processed
static clock_t prev_time = 0;

// Function which returns image formats mapped between libcamera and qt
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

// Callback function which processes the request once it has been completed by libcamera
static void requestComplete(Request *request)
{
    // Check if the request has been cancelled
    if (request->status() == Request::RequestCancelled)
    {
        return;
    }

    // Extract the buffers filled with images from Request object passed by libcamera 
    const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

    // Iterate over the buffer pairs
    for (auto bufferPair : buffers)
    {
        // Use framebuffer which has the image data
        FrameBuffer *buffer = bufferPair.second;

        // Use the frame metadata
        const FrameMetadata &metadata = buffer->metadata();
        std::cout << " seq: " << std::setw(6) << std::setfill('0') << metadata.sequence << " bytesused: ";

        // Calculate the amount of storage used for a single frame
        unsigned int nplane = 0;
        for (const FrameMetadata::Plane &plane : metadata.planes)
        {
            std::cout << plane.bytesused;
            if (++nplane < metadata.planes.size())
                std::cout << "/";
        }
        std::cout << std::endl;

        // Find the size of buffer
        size_t size = buffer->metadata().planes[0].bytesused;
        const FrameBuffer::Plane &plane = buffer->planes().front();
        void *memory = mmap(NULL, plane.length, PROT_READ, MAP_SHARED, plane.fd.fd(), 0);

        // Load image from a raw buffer into the QImage widget
        viewfinder.loadFromData(static_cast<unsigned char *>(memory), (int)size);

        // Record the current time and find the time elapsed between two frames
        // to calculate the FPS
        clock_t current_time = std::clock();
        std::string fps_string = "FPS: " + std::to_string(CLOCKS_PER_SEC / (float)(current_time - prev_time));
        prev_time = current_time;

        // Display the FPS
        QPainter fps_label(&viewfinder);
        fps_label.setPen(QPen(Qt::black));
        fps_label.setFont(QFont("Times", 18, QFont::Bold));
        fps_label.drawText(viewfinder.rect(), Qt::AlignBottom | Qt::AlignLeft, QString::fromStdString(fps_string));

        // Set the label and show it
        viewfinder_label->setPixmap(QPixmap::fromImage(viewfinder));
        viewfinder_label->show();
    }

    // Indicate that we want to reuse the same buffers passed earlier
    request->reuse(Request::ReuseBuffers);
    // Queue a new request for frames to libcamera
    camera->queueRequest(request);
}

int main(int argc, char *argv[])
{   
    // QAppplication main window object
    QApplication window(argc, argv);
    // alloc memory for viewfinder_label
    viewfinder_label = new QLabel;

    // Start the camera manager which handles all the camera's in the syste,
    libcamera::CameraManager *cm = new libcamera::CameraManager();
    cm->start();

    // Display all the camera's connected to the system
    for (auto const &camera : cm->cameras())
    {
        std::cout << "Camera ID: " << camera->id() << std::endl;
    }

    // Extract id of the first camera detected
    std::string camera_id = cm->cameras()[0]->id();
    // Get the camera object connected to the camera
    camera = cm->get(camera_id);
    // Acquire the camera so no other process can use it
    camera->acquire();

    // Config the camera with Raw StreamRole
    std::unique_ptr<libcamera::CameraConfiguration> config = camera->generateConfiguration({libcamera::StreamRole::Raw});

    // Get the StreamConfiguration Object so that we can change it's parameters
    libcamera::StreamConfiguration &streamConfig = config->at(0);

    // Try to set the pixel format given by libcamera to one that is compatible
    // with QImage widget
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

    // Validate the current configuration is correct
    config->validate();
    std::cout << "Default viewfinder configuration is: " << streamConfig.toString() << std::endl;
    // Set the config to the camera
    camera->configure(config.get());

    // Allocator object which is used to allocated framebuffers for eah stream
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

    // Get the actual object which represents a stream
    Stream *stream = streamConfig.stream();
    // Get the framebuffers allocated for the stream
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
    // Create a vector for storing requests which will be queued laterop
    std::vector<std::unique_ptr<Request>> requests;

    // Iterate over buffers to create requests for each buffer and push the
    // requests into an vector
    for (const std::unique_ptr<FrameBuffer> &buffer : buffers)
    {
        std::unique_ptr<Request> request = camera->createRequest();
        if (!request)
        {
            std::cerr << "Can't create request" << std::endl;
            return -ENOMEM;
        }

        int ret = request->addBuffer(stream, buffer.get());
        if (ret < 0)
        {
            std::cerr << "Can't set buffer for request"
                      << std::endl;
            return ret;
        }

        requests.push_back(std::move(request));
    }

    // Register callback function for request completed signal
    camera->requestCompleted.connect(requestComplete);

    // Finally start the camera
    camera->start();
    // Iterate over the generated requests and queue them to libcamera to be fulfilled
    for (std::unique_ptr<libcamera::Request> &request : requests)
    {
        camera->queueRequest(request.get());
    }

    // Qt window handler
    int ret = window.exec();

    // deallocate after closing window
    camera->stop();
    allocator->free(stream);
    delete allocator;
    camera->release();
    camera.reset();
    cm->stop();
    delete viewfinder_label;

    return ret;
}
