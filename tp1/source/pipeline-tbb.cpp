//#include <tbb/pipeline.h>
//#include <tbb/parallel_for_each.h>
#include "tbb/parallel_for_each.h"
#include <vector>

namespace tbb {

}

extern "C" {

#include "filter.h"
#include "log.h"
#include "pipeline.h"

class ImageUniquePtr
{
    image_t* image_;

public:
    ImageUniquePtr(image_t* image)
    :image_(image)
    {}

    ImageUniquePtr(ImageUniquePtr&& other)
    :image_(other.image_) {
        other.image_ = NULL;
    }

    ~ImageUniquePtr() {
        if(image_ != NULL)
            image_destroy(image_);
    }

    image_t* get() {
        return image_;
    }
};

class ImageConverter
{
    image_dir_t* image_dir_;

public:
    ImageConverter(image_dir_t* image_dir)
    :image_dir_(image_dir)
    {}

    void operator() (ImageUniquePtr& imagePtr) const
    {
        {
            image_t* image2 = filter_scale_up(imagePtr.get(), 2);
            //printf("finished image2\n");
            if (image2 == NULL) {
                goto fail_exit;
            }

            image_t* image3 = filter_desaturate(image2);
            //printf("finished image3\n");
            image_destroy(image2);
            if (image3 == NULL) {
                goto fail_exit;
            }

            image_t* image4 = filter_horizontal_flip(image3);
            //printf("finished image4\n");
            image_destroy(image3);
            if (image4 == NULL) {
                goto fail_exit;
            }

            image_t* image5 = filter_sobel(image4);
            //printf("finished image5\n");
            image_destroy(image4);
            if (image5 == NULL) {
                goto fail_exit;
            }

            image_dir_save(image_dir_, image5);
            //printf("finished saving\n");
            printf(".");
            fflush(stdout);
            image_destroy(image5);
            return;
        }
    fail_exit:
        throw std::runtime_error("image generation has failed");
    }
};

std::vector<ImageUniquePtr> get_images(image_dir_t* image_dir)
{
    std::vector<ImageUniquePtr> images;

    for(
        image_t* image = image_dir_load_next(image_dir);
        image != NULL;
        image = image_dir_load_next(image_dir)
    ) {
        images.push_back(image);
    }

    return images;
}

int pipeline_tbb(image_dir_t* image_dir) {
    
    try
    {
        auto images = get_images(image_dir);
        tbb::parallel_for_each(images.begin(), images.end(), ImageConverter(image_dir));
        return 0;
    }
    catch(const std::exception& e)
    {
        printf("%s\n",e.what());
        return -1;
    }
}

} /* extern "C" */
