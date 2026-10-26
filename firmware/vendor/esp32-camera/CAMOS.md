# esp32-camera for CamOS

espressif/esp32-camera 2.1.8 (Apache-2.0, see LICENSE) without its examples and tests, with one addition:

- `esp_camera_set_dma_buffer(buf, size)` (driver/include/esp_camera.h, driver/cam_hal.c): a DMA buffer the
  application keeps. In the internal DMA mode the driver uses it instead of allocating one when it is large enough,
  and does not free it. CamOS gives it a static 16 KB block (main/cam.c), so a fragmented internal heap never stops
  a camera start ("DMA buffer 16384 Byte malloc failed, the current largest free block: 13312 Byte").

main/idf_component.yml points espressif/esp32-camera here (override_path). To move to a newer release, copy it here
and apply the same change (search for "CamOS").
