#pragma once

#include "StationDataModel.hpp"
#include <functional>

namespace avscan {
namespace terrestrial {

struct MotorCapabilities {
    bool has_programmatic_connection{false};
    bool has_measured_angle_feedback{false};
    bool can_command_angle{false};
    bool can_command_continuous_speed{false};
    bool has_hardware_emergency_stop{false};
    double max_speed_deg_per_sec{30.0};
    double min_angular_resolution_deg{0.1};
    std::string device_model{"Manual Detent Indexer"};
};

class IRotationController {
public:
    virtual ~IRotationController() = default;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    virtual MotorCapabilities getCapabilities() const = 0;
    virtual RotationMode getRotationMode() const = 0;

    virtual bool setZeroPosition() = 0;
    virtual bool moveToAngleDeg(double target_deg) = 0;
    virtual bool moveRelativeDeg(double delta_deg) = 0;
    virtual void stopMotion() = 0;

    virtual MotionState getMotionState() const = 0;
    virtual bool getCurrentAngleDeg(double& out_angle_deg) const = 0;

    // Operator notification callback for Manual / Preset steps
    using PromptOperatorCallback = std::function<void(const std::string& prompt_msg, double target_angle_deg)>;
    virtual void setOperatorPromptCallback(PromptOperatorCallback cb) = 0;
};

// Standard Default Manual Indexer (T1–T4 baseline, operates without Bluetooth / physical motor drivers)
class ManualRotationController : public IRotationController {
public:
    ManualRotationController() : current_angle_deg_(0.0), state_(MotionState::STATIONARY) {}

    bool connect() override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }

    MotorCapabilities getCapabilities() const override {
        MotorCapabilities cap;
        cap.has_programmatic_connection = false;
        cap.has_measured_angle_feedback = false;
        cap.can_command_angle = false;
        cap.device_model = "Manual Tripod Head / Turntable Detent";
        return cap;
    }

    RotationMode getRotationMode() const override { return RotationMode::MANUAL_INDEX; }

    bool setZeroPosition() override {
        current_angle_deg_ = 0.0;
        state_ = MotionState::STATIONARY;
        return true;
    }

    bool moveToAngleDeg(double target_deg) override {
        current_angle_deg_ = target_deg;
        state_ = MotionState::STATIONARY;
        if (prompt_cb_) {
            prompt_cb_("Rotate scanner to target position and confirm stillness.", target_deg);
        }
        return true;
    }

    bool moveRelativeDeg(double delta_deg) override {
        return moveToAngleDeg(current_angle_deg_ + delta_deg);
    }

    void stopMotion() override {
        state_ = MotionState::STATIONARY;
    }

    MotionState getMotionState() const override { return state_; }
    void setMotionState(MotionState state) { state_ = state; }

    bool getCurrentAngleDeg(double& out_angle_deg) const override {
        out_angle_deg = current_angle_deg_;
        return false; // nominal only, not measured encoder
    }

    void setOperatorPromptCallback(PromptOperatorCallback cb) override {
        prompt_cb_ = std::move(cb);
    }

private:
    double current_angle_deg_;
    MotionState state_;
    PromptOperatorCallback prompt_cb_;
};

} // namespace terrestrial
} // namespace avscan
