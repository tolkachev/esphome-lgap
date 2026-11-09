#pragma once
#include <vector>
#include <stdint.h>
#include "esphome/core/hal.h"
#include "lgap.h"

namespace esphome
{
  namespace lgap
  {
    class LGAP;

    class LGAPDevice : public Component
    {
      public:
        // float get_setup_priority() const override;
        bool write_update_pending{false};
        // LGAP.pdf: send a control frame (EXE=1) more than 3 times, then go back to monitoring frames
        uint8_t write_repeats_left{0};
        // state reported by the unit before this time may predate the last command, so it is not applied
        uint32_t state_hold_until{0};

        void request_write()
        {
          this->write_update_pending = true;
          this->write_repeats_left = 3;
        }
        bool is_state_held() const { return this->write_update_pending || (int32_t)(this->state_hold_until - millis()) > 0; }

        void set_parent(LGAP *parent) { parent_ = parent; }
        void set_zone_number(int zone_number) { this->zone_number = zone_number; }

        void on_message_received(std::vector<uint8_t> &message);
        void generate_lgap_request(std::vector<uint8_t> &message, uint8_t &request_id);
        
        // uint32_t last_uart_update_time_{0};
        // uint32_t last_ha_update_time_{0};

      protected:
        friend LGAP;
        LGAP *parent_;

        int zone_number{-1};

        virtual void handle_on_message_received(std::vector<uint8_t> &message) = 0;
        virtual void handle_generate_lgap_request(std::vector<uint8_t> &message, uint8_t &request_id) = 0;
    };

  } // namespace lgap
} // namespace esphome