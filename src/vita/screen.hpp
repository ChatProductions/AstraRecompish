#pragma once

#include <cstdint>

class Screen {
public:
  bool init();
  void shutdown();
  void clear(std::uint32_t color);
  void rect(int x, int y, int width, int height, std::uint32_t color);
  void text(int x, int y, const char* value, std::uint32_t color, int scale = 2);
  void blit_160x112(const std::uint32_t* source, int y_offset = 48);
  void present();

private:
  // Draw into pixels_ while front_ is displayed. Swap only on VBlank.
  std::uint32_t* pixels_ = nullptr;
  std::uint32_t* front_ = nullptr;
  int memblock_ = -1;
};
