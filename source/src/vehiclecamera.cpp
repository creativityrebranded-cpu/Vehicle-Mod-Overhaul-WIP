#include "vehiclecamera.h"
#include "vehicleflight.h"
#include "cameraroll.h"
#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include <Windows.h>
#include <atomic>
#include <mutex>

namespace vehiclecamera
{
    namespace {
        using Euler = void(*)(float*, float, float, float);
        Euler g_euler = nullptr;
        std::atomic<bool> g_ready{false};
        std::mutex g_lock;
        Attitude g_attitude;
        ULONGLONG g_at = 0;
        unsigned g_generation = 0;
        std::atomic<unsigned> g_calls[2]{},g_applied[2]{};
        std::atomic<float> g_lastBank[2]{};
        ULONGLONG g_reportAt=0;
        std::uintptr_t g_reportVehicle=0; // LogStatus is main-thread only
        struct View { cameraroll::Filter filter; LARGE_INTEGER at{}; unsigned generation = 0; } g_views[2];

        bool VehicleView() {
            __try {
                const auto camera = *reinterpret_cast<std::uintptr_t*>(game::Base()+addr::kPlayerCameraPtr);
                const auto state = camera ? *reinterpret_cast<std::uintptr_t*>(camera+0x10) : 0;
                return state && *reinterpret_cast<std::uintptr_t*>(state)==game::Base()+addr::kVehicleCameraStateVtbl;
            } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        struct CameraReading {
            std::uintptr_t state=0,vtable=0;
            unsigned mode=0,previous=0;
            bool rows=false;
            float rotation[9]{};
        };
        bool ReadCamera(CameraReading& out) {
            __try {
                const auto camera=*reinterpret_cast<std::uintptr_t*>(game::Base()+addr::kPlayerCameraPtr);
                if (!camera) return false;
                out.state=*reinterpret_cast<std::uintptr_t*>(camera+0x10);
                if (out.state) {
                    out.vtable=*reinterpret_cast<std::uintptr_t*>(out.state);
                    if (out.vtable==game::Base()+addr::kVehicleCameraStateVtbl) {
                        out.mode=*reinterpret_cast<std::uint8_t*>(out.state+0x74);
                        out.previous=*reinterpret_cast<std::uint8_t*>(out.state+0x138);
                    }
                }
                const auto root=*reinterpret_cast<std::uintptr_t*>(camera+8);
                if (root) {
                    out.rows=true;
                    for(int r=0;r<3;++r) for(int c=0;c<3;++c) {
                        const float value=*reinterpret_cast<float*>(root+0x80+r*0x10+c*4);
                        out.rotation[r*3+c]=value; out.rows=out.rows&&std::isfinite(value);
                    }
                }
                return true;
            } __except(EXCEPTION_EXECUTE_HANDLER) { out.rows=false; return false; }
        }
        void Apply(float* out, int view) {
            g_calls[view].fetch_add(1,std::memory_order_relaxed);
            Attitude a;
            const bool active = g_ready.load(std::memory_order_acquire) && ReadAttitude(a);
            LARGE_INTEGER now{}, frequency{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
            double bank = 0;
            {
                std::lock_guard lk(g_lock);
                auto& v = g_views[view];
                if (!active) { v = {}; g_lastBank[view].store(0,std::memory_order_relaxed); return; }
                if (v.generation != g_generation) { v = {}; v.generation = g_generation; }
                const double dt = v.at.QuadPart && frequency.QuadPart ? double(now.QuadPart-v.at.QuadPart)/double(frequency.QuadPart) : 1.0/60;
                bank = v.filter.Step(a.roll, dt); v.at = now;
            }
            cameraroll::Apply(out, bank);
            g_lastBank[view].store(float(bank),std::memory_order_relaxed);
            g_applied[view].fetch_add(1,std::memory_order_relaxed);
        }
        void ThirdPerson(float* out, float yaw, float pitch, float roll) {
            g_euler(out,yaw,pitch,roll); Apply(out,0);
        }
        void FirstPerson(float* out, float yaw, float pitch, float roll) {
            // Native first person composes E * chassis^T * smoothedChassis; bank E once,
            // keeping the native chassis cancellation and seat/movement smoothing intact.
            g_euler(out,yaw,pitch,roll); Apply(out,1);
        }
    }
    bool ReadAttitude(Attitude& out) {
        ULONGLONG at;
        { std::lock_guard lk(g_lock); out=g_attitude; at=g_at; }
        return out.vehicle && GetTickCount64()-at<=500 && vehicleflight::PhysicalCameraActive(out.vehicle) && VehicleView() &&
            std::isfinite(out.pitch) && std::isfinite(out.roll);
    }
    void Publish(std::uintptr_t vehicle, double pitch, double roll) {
        std::lock_guard lk(g_lock);
        if (vehicle!=g_attitude.vehicle) ++g_generation;
        g_attitude={vehicle,pitch,roll}; g_at=GetTickCount64();
    }
    void Reset() {
        std::lock_guard lk(g_lock);
        g_attitude={}; g_at=0; ++g_generation;
    }
    void LogStatus() {
        if (!logger::Verbose()) return;
        Attitude a; ULONGLONG at;
        { std::lock_guard lk(g_lock); a=g_attitude; at=g_at; }
        if (!a.vehicle&&!g_reportVehicle) return;
        const auto now=GetTickCount64();
        if (a.vehicle==g_reportVehicle&&now-g_reportAt<1000) return;
        g_reportAt=now; g_reportVehicle=a.vehicle;
        CameraReading camera; const bool read=ReadCamera(camera);
        const bool owned=vehicleflight::PhysicalCameraActive(a.vehicle),fresh=now-at<=500;
        const bool view=read&&camera.vtable==game::Base()+addr::kVehicleCameraStateVtbl;
        const double degrees=180/cameraroll::kPi;
        const double actualRoll=camera.rows?std::atan2(-double(camera.rotation[2]),double(camera.rotation[8]))*degrees:0;
        const double actualPitch=camera.rows?std::asin(std::clamp(double(camera.rotation[5]),-1.0,1.0))*degrees:0;
        const auto thirdCalls=g_calls[0].exchange(0),firstCalls=g_calls[1].exchange(0);
        const auto thirdApplied=g_applied[0].exchange(0),firstApplied=g_applied[1].exchange(0);
        logger::Write("Physical VTOL camera: state %p vtable %p vehicle-view %d mode %u previous %u; owned %d fresh %d; "
            "callbacks applied/total TP %u/%u FP %u/%u; body roll %.1f pitch %.1f; applied TP %.1f FP %.1f; root %s roll %.1f pitch %.1f deg "
            "R(%.3f %.3f %.3f) F(%.3f %.3f %.3f) U(%.3f %.3f %.3f)",
            reinterpret_cast<void*>(camera.state),reinterpret_cast<void*>(camera.vtable),view,camera.mode,camera.previous,owned,fresh,
            thirdApplied,thirdCalls,firstApplied,firstCalls,a.roll*degrees,a.pitch*degrees,
            g_lastBank[0].load()*degrees,g_lastBank[1].load()*degrees,camera.rows?"ok":"unreadable",actualRoll,actualPitch,
            camera.rotation[0],camera.rotation[1],camera.rotation[2],camera.rotation[3],camera.rotation[4],camera.rotation[5],camera.rotation[6],camera.rotation[7],camera.rotation[8]);
    }
    bool Install() {
        g_euler=reinterpret_cast<Euler>(game::Base()+0x02BCE350);
        const bool third=hook::RedirectCall(game::Base()+0x00FB6CA2,reinterpret_cast<void*>(g_euler),reinterpret_cast<void*>(&ThirdPerson));
        const bool first=hook::RedirectCall(game::Base()+0x00FB68C4,reinterpret_cast<void*>(g_euler),reinterpret_cast<void*>(&FirstPerson));
        g_ready.store(third&&first,std::memory_order_release);
        logger::Write("Physical VTOL camera: %s (third %d, first %d); smoothed full bank, vehicle view only",third&&first?"ready":"disabled",third,first);
        return third&&first;
    }
}
