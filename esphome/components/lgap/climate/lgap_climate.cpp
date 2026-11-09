#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "../lgap.h"
#include "lgap_climate.h"

namespace esphome
{
  namespace lgap
  {

    static const char *const TAG = "lgap.climate";
    static const uint8_t MIN_TEMPERATURE = 16;
    static const uint8_t MAX_TEMPERATURE = 36;

    // LGAP room temperature table (LGAP.pdf, "3. Room Temperature Table"):
    // first raw value of each degree, from 40 °C (raw 76) down to 10 °C (raw 160..162)
    static const uint8_t ROOM_TEMPERATURE_RAW[] = {
        76, 78, 80, 83, 85, 87, 90, 93, 95, 98, 100, 103, 106, 109, 112, 114,
        117, 120, 123, 126, 129, 132, 135, 138, 142, 145, 148, 151, 154, 157, 160};
    static const uint8_t ROOM_TEMPERATURE_RAW_END = 163; // first raw value after 10 °C
    static const int ROOM_TEMPERATURE_MAX = 40;

    // A degree spans 2-4 raw values: interpolate inside it and round to 0.5 °C,
    // so a room sitting on a degree boundary reads e.g. 21.5 instead of flicking 21/22.
    static float decode_room_temperature(uint8_t raw)
    {
      const size_t count = sizeof(ROOM_TEMPERATURE_RAW);
      if (raw < ROOM_TEMPERATURE_RAW[0])
        return ROOM_TEMPERATURE_MAX;

      size_t i = 0;
      while (i + 1 < count && raw >= ROOM_TEMPERATURE_RAW[i + 1])
        i++;

      uint8_t start = ROOM_TEMPERATURE_RAW[i];
      uint8_t end = (i + 1 < count) ? ROOM_TEMPERATURE_RAW[i + 1] : ROOM_TEMPERATURE_RAW_END;
      if (raw >= end)
        return ROOM_TEMPERATURE_MAX - (float)i;

      float temperature = (ROOM_TEMPERATURE_MAX - (float)i) + 0.5f - (raw - start + 0.5f) / (end - start);
      return roundf(temperature * 2) / 2;
    }

    // LGAP.pdf fan speed: Low(1), Middle(2), High(3), Auto(4)
    static const uint8_t FAN_SPEED_LOW = 1;
    static const uint8_t FAN_SPEED_MEDIUM = 2;
    static const uint8_t FAN_SPEED_HIGH = 3;
    static const uint8_t FAN_SPEED_AUTO = 4;

    void LGAPHVACClimate::dump_config()
    {
      ESP_LOGCONFIG(TAG, "LGAP HVAC:");
      ESP_LOGCONFIG(TAG, "  Zone Number: %d", this->zone_number);
      ESP_LOGCONFIG(TAG, "  Mode: %d", (int)this->mode);
      ESP_LOGCONFIG(TAG, "  Swing: %d", (int)this->swing_mode);
      ESP_LOGCONFIG(TAG, "  Temperature: %d", this->target_temperature);
    }

    void LGAPHVACClimate::setup()
    {
      ESP_LOGCONFIG(TAG, "setup() setting initial HVAC state...");

      // restore set points
      auto restore = this->restore_state_();
      if (restore.has_value())
      {
        ESP_LOGCONFIG(TAG, "Restoring original state...");
        restore->apply(this);
      }
      else
      {
        ESP_LOGCONFIG(TAG, "Creating new state...");
        // restore from defaults
        this->mode = climate::CLIMATE_MODE_OFF;

        // initialize target temperature to some value so that it's not NAN
        this->target_temperature = roundf(clamp(this->current_temperature, (float)MIN_TEMPERATURE, (float)MAX_TEMPERATURE));
        this->fan_mode = climate::CLIMATE_FAN_AUTO;
        this->swing_mode = climate::CLIMATE_SWING_OFF;
        this->preset = climate::CLIMATE_PRESET_NONE;
      }

      // Never send nan to HA
      if (std::isnan(this->target_temperature))
      {
        this->target_temperature = 24;
      }

      // todo: initialise the current temp too
    }

    esphome::climate::ClimateTraits LGAPHVACClimate::traits()
    {
      auto traits = climate::ClimateTraits();
      traits.set_supports_current_temperature(true);
      traits.set_supports_two_point_target_temperature(false);
      traits.set_supports_current_humidity(false);
      traits.set_supports_target_humidity(false);

      traits.set_supported_modes({
          climate::CLIMATE_MODE_OFF,
          climate::CLIMATE_MODE_HEAT,
          climate::CLIMATE_MODE_DRY,
          climate::CLIMATE_MODE_COOL,
          climate::CLIMATE_MODE_FAN_ONLY,
          climate::CLIMATE_MODE_HEAT_COOL,
      });

      traits.set_supported_fan_modes({
          climate::CLIMATE_FAN_AUTO,
          climate::CLIMATE_FAN_LOW,
          climate::CLIMATE_FAN_MEDIUM,
          climate::CLIMATE_FAN_HIGH,
      });

      traits.set_supported_swing_modes({
          climate::CLIMATE_SWING_OFF,
          climate::CLIMATE_SWING_VERTICAL,
      });

      // todo: validate these min/max numbers
      traits.set_visual_min_temperature(MIN_TEMPERATURE);
      traits.set_visual_max_temperature(MAX_TEMPERATURE);
      traits.set_visual_target_temperature_step(1);
      traits.set_visual_current_temperature_step(0.5);
      return traits;
    }

    void LGAPHVACClimate::control(const esphome::climate::ClimateCall &call)
    {
      ESP_LOGD(TAG, "esphome::climate::ClimateCall");

      // mode
      if (call.get_mode().has_value())
      {
        ESP_LOGD(TAG, "Mode change requested");
        climate::ClimateMode mode = *call.get_mode();

        // mode - LGAP has a separate state for power and for mode. HA combines them into a single entity
        // anything that is not Off, needs to also set the power mode to On
        if (this->mode != mode)
        {

          if (mode == climate::CLIMATE_MODE_OFF)
          {
            this->power_state_ = 0;
          }
          else if (mode == climate::CLIMATE_MODE_HEAT)
          {
            this->power_state_ = 1;
            this->mode_ = 4;
          }
          else if (mode == climate::CLIMATE_MODE_DRY)
          {
            this->power_state_ = 1;
            this->mode_ = 1;
          }
          else if (mode == climate::CLIMATE_MODE_COOL)
          {
            this->power_state_ = 1;
            this->mode_ = 0;
          }
          else if (mode == climate::CLIMATE_MODE_FAN_ONLY)
          {
            this->power_state_ = 1;
            this->mode_ = 2;
          }
          else if (mode == climate::CLIMATE_MODE_HEAT_COOL)
          {
            this->power_state_ = 1;
            this->mode_ = 3;
          }
        }

        // Publish updated state
        this->request_write();
        this->mode = mode;
        this->publish_state();
      }

      // fan speed
      if (call.get_fan_mode().has_value())
      {
        ESP_LOGD(TAG, "Fan speed change requested");
        climate::ClimateFanMode fan_mode = *call.get_fan_mode();

        if (this->fan_mode != fan_mode)
        {
          if (fan_mode == climate::CLIMATE_FAN_AUTO)
          {
            this->fan_speed_ = FAN_SPEED_AUTO;
          }
          else if (fan_mode == climate::CLIMATE_FAN_LOW)
          {
            this->fan_speed_ = FAN_SPEED_LOW;
          }
          else if (fan_mode == climate::CLIMATE_FAN_MEDIUM)
          {
            this->fan_speed_ = FAN_SPEED_MEDIUM;
          }
          else if (fan_mode == climate::CLIMATE_FAN_HIGH)
          {
            this->fan_speed_ = FAN_SPEED_HIGH;
          }

          // publish state
          this->request_write();
          this->fan_mode = fan_mode;
          this->publish_state();
        }
      }

      // swing
      if (call.get_swing_mode().has_value())
      {
        ESP_LOGD(TAG, "Swing mode change requested");
        climate::ClimateSwingMode swing_mode = *call.get_swing_mode();

        if (this->swing_mode != swing_mode)
        {
          if (swing_mode == climate::CLIMATE_SWING_OFF)
          {
            this->swing_ = 0;
          }
          else if (swing_mode == climate::CLIMATE_SWING_VERTICAL)
          {
            this->swing_ = 1;
          }

          // publish state
          this->request_write();
          this->swing_mode = swing_mode;
          this->publish_state();
        }
      }

      // target temperature
      if (call.get_target_temperature().has_value())
      {
        // TODO: enable precision decimals as a yaml setting
        ESP_LOGD(TAG, "Temperature change requested");
        float temp = *call.get_target_temperature();
        if (temp != this->target_temperature_)
        {
          this->target_temperature_ = temp;
          this->target_temperature = temp;
        }

        this->request_write();
        this->publish_state();
      }
    }

    void LGAPHVACClimate::handle_generate_lgap_request(std::vector<uint8_t> &message, uint8_t &request_id)
    {
      ESP_LOGD(TAG, "Generating %s request message for zone %d...", (this->write_update_pending ? "WRITE" : "READ"), this->zone_number);

      // only create a write request if there is a pending message
      int write_state = this->write_update_pending ? 2 : 0;

      // build payload in message buffer
      message.push_back(this->parent_->get_tx_byte_0());
      message.push_back(0);
      message.push_back(request_id);
      message.push_back(this->zone_number);
      message.push_back(write_state | this->power_state_);
      message.push_back(this->mode_ | (this->swing_ << 3) | (this->fan_speed_ << 4));
      message.push_back((uint8_t)(this->target_temperature_ - 15));

      // blank val for checksum then replace with calculation
      message.push_back(0);
      message[7] = this->parent_->calculate_checksum(message);
    }

    // todo: add handling for when mode change is requested but mode is already on with another zone, ie can't choose heat when cool is already on
    void LGAPHVACClimate::handle_on_message_received(std::vector<uint8_t> &message)
    {
      ESP_LOGD(TAG, "Processing climate message...");

      // handle bad class config
      if (this->zone_number < 0)
        return;
      if (message[4] != zone_number)
        return;

      bool publish_update = false;

      // while a command is on its way (and shortly after), the unit can still report the state
      // from before it: applying that would drop the command, so only the room temperature is taken
      bool state_held = this->is_state_held();
      if (state_held)
        ESP_LOGD(TAG, "Command pending for zone %d, not applying the reported state", this->zone_number);

      // process clean message as checksum already checked before reaching this point
      uint8_t power_state = (message[1] & 1);
      uint8_t mode = (message[6] & 7);

      // power state and mode
      // home assistant climate treats them as a single entity
      // this logic combines them from lgap into a single entity
      if (!state_held && (power_state != this->power_state_ || mode != this->mode_))
      {
        // handle mode
        if (mode == 0)
        {
          this->mode = climate::CLIMATE_MODE_COOL;
        }
        else if (mode == 1)
        {
          this->mode = climate::CLIMATE_MODE_DRY;
        }
        else if (mode == 2)
        {
          this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        }
        else if (mode == 3)
        {
          // heat/cool is essentially auto
          this->mode = climate::CLIMATE_MODE_HEAT_COOL;
        }
        else if (mode == 4)
        {
          this->mode = climate::CLIMATE_MODE_HEAT;
        }
        else
        {
          ESP_LOGE(TAG, "Invalid mode received: %d", mode);
          this->mode = climate::CLIMATE_MODE_OFF;
        }

        // handle power state
        if (power_state == 0)
        {
          this->mode = climate::CLIMATE_MODE_OFF;
        }

        // update state
        publish_update = true;
        this->mode_ = mode;
        this->power_state_ = power_state;
      }

      // swing options
      uint8_t swing = (message[6] >> 3) & 1;
      if (!state_held && swing != this->swing_)
      {
        if (swing == 0)
        {
          this->swing_mode = climate::CLIMATE_SWING_OFF;
        }
        else if (swing == 1)
        {
          this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
        }
        else
        {
          ESP_LOGE(TAG, "Invalid swing received: %d", swing);
        }

        // update state
        this->swing_ = swing;
        publish_update = true;
      }

      // fan speed
      uint8_t fan_speed = ((message[6] >> 4) & 7);
      if (!state_held && fan_speed != this->fan_speed_)
      {
        if (fan_speed == FAN_SPEED_LOW)
        {
          this->fan_mode = climate::CLIMATE_FAN_LOW;
        }
        else if (fan_speed == FAN_SPEED_MEDIUM)
        {
          this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
        }
        else if (fan_speed == FAN_SPEED_HIGH)
        {
          this->fan_mode = climate::CLIMATE_FAN_HIGH;
        }
        else if (fan_speed == FAN_SPEED_AUTO)
        {
          this->fan_mode = climate::CLIMATE_FAN_AUTO;
        }
        else
        {
          ESP_LOGW(TAG, "Unknown fan speed received: %d", fan_speed);
        }

        // update state
        this->fan_speed_ = fan_speed;
        publish_update = true;
      }

      // target temp
      int target_temperature = (message[7] & 0xf) + 15;
      if (!state_held && target_temperature != this->target_temperature_)
      {
        this->target_temperature_ = target_temperature;
        this->target_temperature = target_temperature;
        publish_update = true;
      }

      // current temp: byte 8 is a raw 0-255 value, mapped by the LGAP room temperature table
      float current_temperature = decode_room_temperature(message[8]);
      ESP_LOGD(TAG, "Current temperature: %.1f (raw %d)", current_temperature, message[8]);
      // checks that temperature is different AND that the publish time interval has passed
      // (the first value after boot is published right away)
      if (current_temperature != this->current_temperature_)
      {
        if (!this->temperature_published_ || this->temperature_last_publish_time_ + this->temperature_publish_time_ <= millis())
        {
          ESP_LOGD(TAG, "Temperature update time has lapsed. Sending update...");
          this->temperature_published_ = true;
          this->temperature_last_publish_time_ = millis();
          this->current_temperature_ = current_temperature;
          this->current_temperature = current_temperature;
          publish_update = true;
        } else {
          ESP_LOGD(TAG, "Temperature update time hasn't lapsed. Ignoring temperature difference...");
        }
      }

      // send update to home assistant with all the changed variables
      if (publish_update == true)
      {
        this->publish_state();
      }
    }
  } // namespace lgap
} // namespace esphome
