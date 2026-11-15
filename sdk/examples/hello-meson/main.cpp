// hello-meson: C++ calling its own C library and RitterOS's libimage
// (dependency(): pkg-config).
#include <format>
#include <iostream>
#include <image.h>
#include "greet.h"

int main(int argc, char **argv)
{
    char text[64];
    greet(text, sizeof(text), argc > 1 ? argv[1] : "world");
    std::cout << "hello-meson: " << text << '\n';
    // RitterOS's libimage: a file that is no image is refused.
    struct image img = {};
    int r = image_load_memory("not an image", 12, &img);
    std::cout << std::format("hello-meson: libimage refused a non-image: {}\n", r != 0);
    image_free(&img);
    return 0;
}
