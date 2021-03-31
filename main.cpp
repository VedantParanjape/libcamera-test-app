#include <iomanip>
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>

#include <libcamera/libcamera.h>
#include "buffer_writer.h"

using namespace libcamera;

std::shared_ptr<libcamera::Camera> camera;

static void requestComplete(Request *request)
{
    BufferWriter writer;
    if (request->status() == Request::RequestCancelled)
    {
        return;
    }

    const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

    int i = 0;
    for (auto bufferPair : buffers)
    {
        const Stream *stream = bufferPair.first;
        FrameBuffer *buffer = bufferPair.second;

		const std::string &name = "Stream" + std::to_string(++i);

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

        writer.mapBuffer(buffer);
		writer.write(buffer, name);
    }

    request->reuse(Request::ReuseBuffers);
    
    if (!request)
    {
        std::cerr << "Can't create request" << std::endl;
        return;
    }

    camera->queueRequest(request);
}

int main()
{
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
    streamConfig.size.width = 640;
    streamConfig.size.height = 480;
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

    camera->start();
    for (std::shared_ptr<libcamera::Request> request : requests)
    {
        camera->queueRequest(request.get());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    camera->stop();
    allocator->free(stream);
    delete allocator;
    camera->release();
    camera.reset();
    cm->stop();

    return 0;
}
