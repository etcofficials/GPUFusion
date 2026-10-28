// GPU Fusion for Godot: a GDExtension that lets a Godot project run image work on every GPU of the PC
// at once, through the GPU Fusion SDK's adaptive scheduler (the discrete GPU does most of it, the
// integrated GPU helps only as far as it measurably pays off). It does not change how Godot renders.
//
//     var fx := GPUFusionCompute.new()
//     var blurred: Image = fx.blur(image, 6)        # RGBA8, every GPU
//     print(fx.last_report())                         # rows per GPU, times, why
#include "engine.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

using namespace godot;

class GPUFusionCompute : public RefCounted {
    GDCLASS(GPUFusionCompute, RefCounted)

public:
    // policy: 0 conservative, 1 balanced, 2 performance; max_gpus: 1 = the primary GPU only.
    bool configure(int policy, int max_gpus)
    {
        std::string error;
        const bool ok = m_engine.configure(policy, max_gpus, &error);
        m_error = String::utf8(error.c_str());
        return ok;
    }
    PackedStringArray adapters()
    {
        if (m_engine.adapters().empty())
            configure(0, 8);
        PackedStringArray out;
        for (const std::string& a : m_engine.adapters())
            out.push_back(String::utf8(a.c_str()));
        return out;
    }
    Ref<Image> blur(const Ref<Image>& image, int radius) { return run(image, radius, false); }
    Ref<Image> blur_reference(const Ref<Image>& image, int radius) { return run(image, radius, true); }
    Dictionary last_report() const
    {
        Dictionary d;
        d["ok"] = m_info.ok;
        d["both_gpus"] = m_info.multi;
        d["wall_ms"] = m_info.wallMs;
        d["overhead_ms"] = m_info.overheadMs;
        d["plan"] = String::utf8(m_info.plan.c_str());
        Array gpus;
        for (std::size_t i = 0; i < m_info.devices.size(); ++i) {
            Dictionary g;
            g["name"] = String::utf8(m_info.devices[i].c_str());
            g["rows"] = int64_t(m_info.rows[i]);
            g["work_ms"] = m_info.workMs[i];
            g["transfer_ms"] = m_info.transferMs[i];
            gpus.push_back(g);
        }
        d["gpus"] = gpus;
        d["error"] = m_error;
        return d;
    }
    String learned_state() const { return String::utf8(m_engine.learned().c_str()); }
    bool restore_state(const String& state) { return m_engine.restore(state.utf8().get_data()); }

protected:
    static void _bind_methods()
    {
        ClassDB::bind_method(D_METHOD("configure", "policy", "max_gpus"), &GPUFusionCompute::configure);
        ClassDB::bind_method(D_METHOD("adapters"), &GPUFusionCompute::adapters);
        ClassDB::bind_method(D_METHOD("blur", "image", "radius"), &GPUFusionCompute::blur);
        ClassDB::bind_method(D_METHOD("blur_reference", "image", "radius"), &GPUFusionCompute::blur_reference);
        ClassDB::bind_method(D_METHOD("last_report"), &GPUFusionCompute::last_report);
        ClassDB::bind_method(D_METHOD("learned_state"), &GPUFusionCompute::learned_state);
        ClassDB::bind_method(D_METHOD("restore_state", "state"), &GPUFusionCompute::restore_state);
    }

private:
    Ref<Image> run(const Ref<Image>& image, int radius, bool cpu)
    {
        if (image.is_null() || image->is_empty()) {
            m_error = "no image";
            return Ref<Image>();
        }
        Ref<Image> src = image;
        if (src->get_format() != Image::FORMAT_RGBA8) {
            src = image->duplicate();
            src->convert(Image::FORMAT_RGBA8);
        }
        const uint32_t w = uint32_t(src->get_width()), h = uint32_t(src->get_height());
        const PackedByteArray in = src->get_data();
        PackedByteArray out;
        out.resize(in.size());
        m_error = String();
        if (cpu) {
            gfgodot::Engine::blurReference(in.ptr(), out.ptrw(), w, h, uint32_t(radius));
        } else if (!m_engine.blur(in.ptr(), out.ptrw(), w, h, uint32_t(radius), &m_info)) {
            m_error = String::utf8(m_info.error.c_str());
            return Ref<Image>();
        }
        return Image::create_from_data(int32_t(w), int32_t(h), false, Image::FORMAT_RGBA8, out);
    }

    gfgodot::Engine m_engine;
    gfgodot::RunInfo m_info;
    String m_error;
};

namespace {
void initialize(ModuleInitializationLevel level)
{
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE)
        GDREGISTER_CLASS(GPUFusionCompute);
}
void uninitialize(ModuleInitializationLevel) {}
} // namespace

extern "C" GDExtensionBool GDE_EXPORT gpufusion_library_init(GDExtensionInterfaceGetProcAddress get_proc_address, GDExtensionClassLibraryPtr library,
                                                              GDExtensionInitialization* initialization)
{
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize);
    init.register_terminator(uninitialize);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
