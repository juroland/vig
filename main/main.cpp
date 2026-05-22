#include "esp_log.h"
#include "nvs_flash.h"
#include "net.hpp"
#include <thread>

using namespace std::chrono_literals;

static const char *TAG = "VIG";

namespace vig
{

    class App
    {
    public:
        App()
        {
            ESP_LOGI(TAG, "Starting device initialization...");
            ESP_LOGI(TAG, "Initializing NVS...");
            esp_err_t ret = nvs_flash_init();
            if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
            {
                ESP_ERROR_CHECK(nvs_flash_erase());
                ret = nvs_flash_init();
            }
            ESP_ERROR_CHECK(ret);

            ESP_LOGI(TAG, "Initializing Ethernet...");
            auto net_res = net::NetworkManager::instance().init_ethernet();
            if (!net_res)
            {
                ESP_LOGE(TAG, "Ethernet init failed");
                abort();
            }
        }
        void run()
        {
            while (true)
            {
                std::this_thread::sleep_for(2000ms);
                ESP_LOGI(TAG, "Loop...");
            }
        }
    };
};

extern "C" void app_main()
{
    vig::App app;
    app.run();
}