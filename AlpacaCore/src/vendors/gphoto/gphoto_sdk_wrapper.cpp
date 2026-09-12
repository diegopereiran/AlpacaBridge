// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/gphoto/gphoto_sdk_wrapper.h>

#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-context.h>
#include <gphoto2/gphoto2-file.h>
#include <gphoto2/gphoto2-list.h>
#include <gphoto2/gphoto2-version.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace alpacacore::vendor::gphoto {

namespace {

constexpr const char* kLogTag = "GPhoto";

[[noreturn]] void throw_gp_error(const std::string& what, int gp_result) {
    throw AlpacaException(what + ": " + gp_result_as_string(gp_result), AlpacaError::DriverException);
}

CameraWidget* find_widget_or_null(CameraWidget* root, const std::string& name) {
    CameraWidget* widget = nullptr;
    if (gp_widget_get_child_by_name(root, name.c_str(), &widget) != GP_OK) {
        return nullptr;
    }
    return widget;
}

} // namespace

class GPhotoSDKWrapper::Impl {
public:
    Impl() : context_(gp_context_new()) {
        if (context_ == nullptr) {
            throw AlpacaException("Failed to create libgphoto2 context", AlpacaError::DriverException);
        }
    }

    ~Impl() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, session] : sessions_) {
            (void)id;
            close_session_locked(session);
        }
        sessions_.clear();
        gp_context_unref(context_);
    }

    struct Session {
        Camera* camera{nullptr};
    };

    std::vector<GPhotoCameraInfo> enumerate_cameras() {
        CameraList* list = nullptr;
        int result = gp_list_new(&list);
        if (result != GP_OK) {
            throw_gp_error("gp_list_new failed", result);
        }

        // gp_camera_autodetect returns the number of cameras found (>= 0) on
        // success, not GP_OK -- e.g. exactly one camera makes it return 1,
        // which is not GP_OK. Only a negative result is a real GP_ERROR_*.
        result = gp_camera_autodetect(list, context_);
        if (result < 0) {
            gp_list_free(list);
            throw_gp_error("gp_camera_autodetect failed", result);
        }

        std::vector<GPhotoCameraInfo> cameras;
        int count = gp_list_count(list);
        for (int i = 0; i < count; ++i) {
            const char* name = nullptr;
            const char* port = nullptr;
            gp_list_get_name(list, i, &name);
            gp_list_get_value(list, i, &port);
            cameras.push_back({name ? name : "", port ? port : ""});
        }
        gp_list_free(list);
        return cameras;
    }

    int open_camera(const std::string& model, const std::string& port) {
        Camera* camera = nullptr;
        int result = gp_camera_new(&camera);
        if (result != GP_OK) {
            throw_gp_error("gp_camera_new failed", result);
        }

        // Explicit model+port init (rather than a bare gp_camera_init auto
        // probe) so opening one detected camera never races another
        // USB-attached camera's enumeration -- same rationale INDI's
        // gphoto_driver.cpp documents for its default path.
        CameraAbilitiesList* abilities_list = nullptr;
        GPPortInfoList* port_info_list = nullptr;
        try {
            result = gp_abilities_list_new(&abilities_list);
            if (result != GP_OK) throw_gp_error("gp_abilities_list_new failed", result);
            result = gp_abilities_list_load(abilities_list, context_);
            if (result != GP_OK) throw_gp_error("gp_abilities_list_load failed", result);

            int model_index = gp_abilities_list_lookup_model(abilities_list, model.c_str());
            if (model_index < GP_OK) {
                throw AlpacaException("Camera model not recognized by libgphoto2: " + model,
                                      AlpacaError::InvalidValue);
            }
            CameraAbilities abilities;
            result = gp_abilities_list_get_abilities(abilities_list, model_index, &abilities);
            if (result != GP_OK) throw_gp_error("gp_abilities_list_get_abilities failed", result);
            result = gp_camera_set_abilities(camera, abilities);
            if (result != GP_OK) throw_gp_error("gp_camera_set_abilities failed", result);

            result = gp_port_info_list_new(&port_info_list);
            if (result != GP_OK) throw_gp_error("gp_port_info_list_new failed", result);
            result = gp_port_info_list_load(port_info_list);
            if (result != GP_OK) throw_gp_error("gp_port_info_list_load failed", result);

            int port_index = gp_port_info_list_lookup_path(port_info_list, port.c_str());
            if (port_index < GP_OK) {
                throw AlpacaException("Camera port not found: " + port, AlpacaError::NotConnected);
            }
            GPPortInfo port_info;
            result = gp_port_info_list_get_info(port_info_list, port_index, &port_info);
            if (result != GP_OK) throw_gp_error("gp_port_info_list_get_info failed", result);
            result = gp_camera_set_port_info(camera, port_info);
            if (result != GP_OK) throw_gp_error("gp_camera_set_port_info failed", result);

            result = gp_camera_init(camera, context_);
            if (result != GP_OK) {
                throw_gp_error("gp_camera_init failed (camera not connected or busy)", result);
            }
        } catch (...) {
            if (port_info_list) gp_port_info_list_free(port_info_list);
            if (abilities_list) gp_abilities_list_free(abilities_list);
            gp_camera_unref(camera);
            throw;
        }
        gp_port_info_list_free(port_info_list);
        gp_abilities_list_free(abilities_list);

        std::lock_guard<std::mutex> lock(mutex_);
        int handle = next_handle_++;
        sessions_[handle] = Session{camera};
        return handle;
    }

    void close_camera(int handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(handle);
        if (it == sessions_.end()) {
            return;
        }
        close_session_locked(it->second);
        sessions_.erase(it);
    }

    std::string get_gphoto_version() {
        const char** info = gp_library_version(GP_VERSION_SHORT);
        if (info == nullptr || info[0] == nullptr) {
            return "unknown";
        }
        return info[0];
    }

    std::string get_camera_summary(int handle) {
        Camera* camera = camera_for(handle);
        CameraText text;
        int result = gp_camera_get_summary(camera, &text, context_);
        if (result != GP_OK) {
            throw_gp_error("gp_camera_get_summary failed", result);
        }
        return text.text;
    }

    bool has_widget(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* root = get_config_root(camera);
        bool found = find_widget_or_null(root, name) != nullptr;
        gp_widget_free(root);
        return found;
    }

    std::vector<std::string> get_choices(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* root = get_config_root(camera);
        CameraWidget* widget = find_widget_or_null(root, name);
        std::vector<std::string> choices;
        if (widget != nullptr) {
            int count = gp_widget_count_choices(widget);
            for (int i = 0; i < count; ++i) {
                const char* choice = nullptr;
                if (gp_widget_get_choice(widget, i, &choice) == GP_OK && choice != nullptr) {
                    choices.emplace_back(choice);
                }
            }
        }
        gp_widget_free(root);
        return choices;
    }

    std::string get_choice_value(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* root = get_config_root(camera);
        CameraWidget* widget = find_widget_or_null(root, name);
        if (widget == nullptr) {
            gp_widget_free(root);
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        char* value = nullptr;
        int result = gp_widget_get_value(widget, &value);
        std::string out = (result == GP_OK && value != nullptr) ? value : "";
        gp_widget_free(root);
        if (result != GP_OK) {
            throw_gp_error("gp_widget_get_value failed for " + name, result);
        }
        return out;
    }

    void set_choice_value(int handle, const std::string& name, const std::string& value) {
        Camera* camera = camera_for(handle);
        CameraWidget* root = get_config_root(camera);
        CameraWidget* widget = find_widget_or_null(root, name);
        if (widget == nullptr) {
            gp_widget_free(root);
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        int result = gp_widget_set_value(widget, value.c_str());
        if (result == GP_OK) {
            result = gp_camera_set_config(camera, root, context_);
        }
        gp_widget_free(root);
        if (result != GP_OK) {
            throw_gp_error("Failed to set " + name + " = " + value, result);
        }
    }

    bool get_toggle_value(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* root = get_config_root(camera);
        CameraWidget* widget = find_widget_or_null(root, name);
        if (widget == nullptr) {
            gp_widget_free(root);
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        int toggle_value = 0;
        int result = gp_widget_get_value(widget, &toggle_value);
        gp_widget_free(root);
        if (result != GP_OK) {
            throw_gp_error("gp_widget_get_value failed for " + name, result);
        }
        return toggle_value != 0;
    }

    void set_toggle_value(int handle, const std::string& name, bool on) {
        Camera* camera = camera_for(handle);
        CameraWidget* root = get_config_root(camera);
        CameraWidget* widget = find_widget_or_null(root, name);
        if (widget == nullptr) {
            gp_widget_free(root);
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        int toggle_value = on ? 1 : 0;
        int result = gp_widget_set_value(widget, &toggle_value);
        if (result == GP_OK) {
            result = gp_camera_set_config(camera, root, context_);
        }
        gp_widget_free(root);
        if (result != GP_OK) {
            throw_gp_error(std::string("Failed to set ") + name + (on ? "=1" : "=0"), result);
        }
    }

    std::string get_text_value(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* root = get_config_root(camera);
        CameraWidget* widget = find_widget_or_null(root, name);
        if (widget == nullptr) {
            gp_widget_free(root);
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        char* value = nullptr;
        int result = gp_widget_get_value(widget, &value);
        std::string out = (result == GP_OK && value != nullptr) ? value : "";
        gp_widget_free(root);
        if (result != GP_OK) {
            throw_gp_error("gp_widget_get_value failed for " + name, result);
        }
        return out;
    }

    GPhotoCaptureResult capture_and_download(int handle) {
        Camera* camera = camera_for(handle);
        CameraFilePath path;
        int result = gp_camera_capture(camera, GP_CAPTURE_IMAGE, &path, context_);
        if (result != GP_OK) {
            throw_gp_error("gp_camera_capture failed", result);
        }
        return download_and_delete(camera, path);
    }

    GPhotoCaptureResult wait_for_bulb_file_and_download(int handle) {
        Camera* camera = camera_for(handle);

        // Modern libgphoto2 PTP bulb protocol (both Canon and Nikon ptp2.so
        // camlib expose it identically): the caller flips the "bulb" toggle
        // widget on to open the shutter, sleeps for the requested duration
        // (its own abortable loop -- see gphoto_camera_driver.cpp), then
        // flips it off to close. The camera then posts GP_EVENT_FILE_ADDED
        // with the path of the frame it just wrote; this method waits for
        // that event and downloads the file. Caller has already put the
        // shutter speed widget on its "bulb" choice.
        //
        // TODO(gphoto/nikon): verified against libgphoto2 2.5.31 ptp2.so
        // source/strings only -- not yet run against real Nikon D5300
        // hardware. If the D5300's firmware needs a different bulb sequence
        // (older bodies sometimes require capturetarget=Memory card first,
        // or a settle delay after opening the shutter before it will accept
        // a close), that will surface as a bulb capture timeout in
        // ConformU/hardware validation and should be fixed here.
        //
        // Some bodies also require an explicit gp_camera_capture()-style
        // trigger here instead of a passive event wait; fall back to that if
        // the event wait times out (also a TODO pending hardware access).
        constexpr int kEventWaitTimeoutMs = 15000;
        CameraEventType event_type = GP_EVENT_UNKNOWN;
        void* event_data = nullptr;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kEventWaitTimeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            int result = gp_camera_wait_for_event(camera, 1000, &event_type, &event_data, context_);
            if (result == GP_OK && event_type == GP_EVENT_FILE_ADDED && event_data != nullptr) {
                auto* path = static_cast<CameraFilePath*>(event_data);
                CameraFilePath local_path = *path;
                free(event_data);
                return download_and_delete(camera, local_path);
            }
            if (event_data != nullptr) {
                free(event_data);
                event_data = nullptr;
            }
        }
        throw AlpacaException("Bulb capture: no file-added event from camera within timeout",
                              AlpacaError::DriverException);
    }

private:
    GPContext* context_;
    std::mutex mutex_;
    std::unordered_map<int, Session> sessions_;
    int next_handle_{0};

    // mutex_ only guards the sessions_ map (open/close/lookup bookkeeping).
    // Concurrent libgphoto2 calls against the SAME handle are not serialized
    // here -- exactly like the other camera SDK wrappers (SVBONY, ZWO, ...),
    // it is the owning driver's own mutex_ that guarantees a single caller
    // per handle at a time.
    Camera* camera_for(int handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(handle);
        if (it == sessions_.end()) {
            throw AlpacaException("Unknown gphoto camera handle", AlpacaError::NotConnected);
        }
        return it->second.camera;
    }

    void close_session_locked(Session& session) {
        if (session.camera == nullptr) {
            return;
        }
        gp_camera_exit(session.camera, context_);
        gp_camera_unref(session.camera);
        session.camera = nullptr;
    }

    CameraWidget* get_config_root(Camera* camera) {
        CameraWidget* root = nullptr;
        int result = gp_camera_get_config(camera, &root, context_);
        if (result != GP_OK) {
            throw_gp_error("gp_camera_get_config failed", result);
        }
        return root;
    }

    GPhotoCaptureResult download_and_delete(Camera* camera, const CameraFilePath& path) {
        CameraFile* file = nullptr;
        int result = gp_file_new(&file);
        if (result != GP_OK) {
            throw_gp_error("gp_file_new failed", result);
        }

        result = gp_camera_file_get(camera, path.folder, path.name, GP_FILE_TYPE_NORMAL, file, context_);
        if (result != GP_OK) {
            gp_file_unref(file);
            throw_gp_error("gp_camera_file_get failed", result);
        }

        const char* data = nullptr;
        unsigned long size = 0;
        result = gp_file_get_data_and_size(file, &data, &size);
        if (result != GP_OK) {
            gp_file_unref(file);
            throw_gp_error("gp_file_get_data_and_size failed", result);
        }

        GPhotoCaptureResult out;
        out.filename = path.name;
        out.data.assign(data, data + size);
        const char* mime_type = nullptr;
        if (gp_file_get_mime_type(file, &mime_type) == GP_OK && mime_type != nullptr) {
            out.mime_type = mime_type;
        }
        gp_file_unref(file);

        // Best-effort: leaving frames on the card is harmless but fills it
        // over a long session. Never fail the capture over cleanup.
        int delete_result = gp_camera_file_delete(camera, path.folder, path.name, context_);
        if (delete_result != GP_OK) {
            ALPACA_LOG_DEBUG(kLogTag, std::string("Failed to delete captured file from camera: ") +
                                          gp_result_as_string(delete_result));
        }

        return out;
    }
};

GPhotoSDKWrapper::GPhotoSDKWrapper() : pimpl_(std::make_unique<Impl>()) {}
GPhotoSDKWrapper::~GPhotoSDKWrapper() = default;

GPhotoSDKWrapper& GPhotoSDKWrapper::instance() {
    static GPhotoSDKWrapper wrapper;
    return wrapper;
}

std::vector<GPhotoCameraInfo> GPhotoSDKWrapper::enumerate_cameras() {
    return pimpl_->enumerate_cameras();
}

int GPhotoSDKWrapper::open_camera(const std::string& model, const std::string& port) {
    return pimpl_->open_camera(model, port);
}

void GPhotoSDKWrapper::close_camera(int handle) {
    pimpl_->close_camera(handle);
}

std::string GPhotoSDKWrapper::get_gphoto_version() {
    return pimpl_->get_gphoto_version();
}

std::string GPhotoSDKWrapper::get_camera_summary(int handle) {
    return pimpl_->get_camera_summary(handle);
}

bool GPhotoSDKWrapper::has_widget(int handle, const std::string& name) {
    return pimpl_->has_widget(handle, name);
}

std::vector<std::string> GPhotoSDKWrapper::get_choices(int handle, const std::string& name) {
    return pimpl_->get_choices(handle, name);
}

std::string GPhotoSDKWrapper::get_choice_value(int handle, const std::string& name) {
    return pimpl_->get_choice_value(handle, name);
}

void GPhotoSDKWrapper::set_choice_value(int handle, const std::string& name, const std::string& value) {
    pimpl_->set_choice_value(handle, name, value);
}

bool GPhotoSDKWrapper::get_toggle_value(int handle, const std::string& name) {
    return pimpl_->get_toggle_value(handle, name);
}

void GPhotoSDKWrapper::set_toggle_value(int handle, const std::string& name, bool on) {
    pimpl_->set_toggle_value(handle, name, on);
}

std::string GPhotoSDKWrapper::get_text_value(int handle, const std::string& name) {
    return pimpl_->get_text_value(handle, name);
}

GPhotoCaptureResult GPhotoSDKWrapper::capture_and_download(int handle) {
    return pimpl_->capture_and_download(handle);
}

GPhotoCaptureResult GPhotoSDKWrapper::wait_for_bulb_file_and_download(int handle) {
    return pimpl_->wait_for_bulb_file_and_download(handle);
}

} // namespace alpacacore::vendor::gphoto
