#include <lux/engine/ui/WidgetDispatch.hpp>

#include <lux/engine/platform/FormatCompat.h>

#include <lux/cxx/compile_time/type_info.hpp>

#include <imgui.h>
#include <Eigen/Geometry>

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>

namespace lux::ui
{
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------
    namespace
    {
        /// Parse a float from a string_view using std::from_chars (no allocation).
        static bool parse_float(std::string_view sv, float& out) noexcept
        {
            auto r = std::from_chars(sv.data(), sv.data() + sv.size(), out);
            return r.ec == std::errc{};
        }

        /// Null-terminate a string_view into a fixed buffer (truncates at max_len-1).
        template<std::size_t N>
        static void sv_to_cstr(std::string_view sv, char (&buf)[N]) noexcept
        {
            const std::size_t n = std::min(sv.size(), N - 1);
            std::memcpy(buf, sv.data(), n);
            buf[n] = '\0';
        }

        /// Extract min/max float pair from annotation.  Returns false if not present or
        /// not parseable.
        static bool annotation_range(const lux::meta::AnnotationView& annot,
                                     float& out_min, float& out_max) noexcept
        {
            auto sv_min = annot.get("min");
            auto sv_max = annot.get("max");
            if (!sv_min.has_value() || !sv_max.has_value())
                return false;
            return parse_float(*sv_min, out_min) && parse_float(*sv_max, out_max);
        }

        /// Show a tooltip from the "tooltip" annotation key if the item is hovered.
        static void maybe_tooltip(const lux::meta::AnnotationView& annot) noexcept
        {
            auto tt = annot.get("tooltip");
            if (!tt.has_value()) return;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                char buf[256];
                sv_to_cstr(*tt, buf);
                ImGui::SetTooltip("%s", buf);
            }
        }

        // -----------------------------------------------------------------
        // parse_label_list
        //
        // Splits a comma-separated string_view (e.g. "X,Y,Z") into up to
        // max_n null-terminated strings written into bufs[][8].
        // Returns the number of tokens successfully written.
        // Leading/trailing spaces are trimmed from each token.
        // -----------------------------------------------------------------
        static int parse_label_list(std::string_view sv,
                                    char bufs[][8], int max_n) noexcept
        {
            int count = 0;
            while (count < max_n && !sv.empty())
            {
                auto comma = sv.find(',');
                auto tok   = (comma == std::string_view::npos) ? sv : sv.substr(0, comma);
                while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
                while (!tok.empty() && tok.back()  == ' ') tok.remove_suffix(1);
                const std::size_t len = std::min(tok.size(), std::size_t{7});
                std::memcpy(bufs[count], tok.data(), len);
                bufs[count][len] = '\0';
                ++count;
                if (comma == std::string_view::npos) break;
                sv = sv.substr(comma + 1);
            }
            return count;
        }

        // -----------------------------------------------------------------
        // draw_vec_components
        //
        // Renders n labeled DragFloat inputs horizontally inside the current
        // ImGui cell.  Labels use Unity-style axis colours:
        //   X - red  Y - green  Z - blue  W - purple
        //
        // group_id  : outer PushID scope, usually the field ##id string.
        // arr       : pointer to n floats (read-write).
        // n         : 2, 3 or 4.
        // labels    : array of n short strings (e.g. {"X","Y","Z"}).
        // speed     : DragFloat speed.
        // vmin/vmax : clamping range (ignored when has_range is false).
        // Returns true if any component was modified.
        // -----------------------------------------------------------------
        static bool draw_vec_components(
            const char* group_id,
            float*      arr,
            int         n,
            const char* const labels[],
            float       speed,
            float       vmin,
            float       vmax,
            bool        has_range) noexcept
        {
            static const ImVec4 kColors[4] = {
                {0.90f, 0.30f, 0.30f, 1.f},   // X – red
                {0.35f, 0.80f, 0.35f, 1.f},   // Y – green
                {0.30f, 0.55f, 0.95f, 1.f},   // Z – blue
                {0.75f, 0.60f, 0.90f, 1.f},   // W – purple
            };
            static const char* const kIds[4] = {"##x", "##y", "##z", "##w"};
            constexpr float kGap      = 4.f;
            constexpr float kLabelPad = 2.f;
            constexpr float kMinDrag  = 30.f;

            // Measure total space consumed by [label + pad + gap] so the
            // remaining width can be divided equally among the drag inputs.
            float total_label_w = 0.f;
            for (int i = 0; i < n; ++i) {
                if (i > 0) total_label_w += kGap;
                total_label_w += ImGui::CalcTextSize(labels[i]).x + kLabelPad;
            }
            const float avail  = ImGui::GetContentRegionAvail().x;
            const float drag_w = std::max((avail - total_label_w) / static_cast<float>(n),
                                          kMinDrag);

            bool changed = false;
            ImGui::PushID(group_id);
            for (int i = 0; i < n; ++i) {
                if (i > 0) ImGui::SameLine(0.f, kGap);
                ImGui::PushStyleColor(ImGuiCol_Text, kColors[i]);
                ImGui::TextUnformatted(labels[i]);
                ImGui::PopStyleColor();
                ImGui::SameLine(0.f, kLabelPad);
                ImGui::SetNextItemWidth(drag_w);   // overrides outer SetNextItemWidth
                if (has_range)
                    changed |= ImGui::DragFloat(kIds[i], &arr[i], speed, vmin, vmax, "%.3f");
                else
                    changed |= ImGui::DragFloat(kIds[i], &arr[i], speed, 0.f, 0.f, "%.3f");
            }
            ImGui::PopID();
            return changed;
        }
    } // anonymous namespace

    // -------------------------------------------------------------------------
    // WidgetDispatch
    // -------------------------------------------------------------------------
    void WidgetDispatch::registerWidget(uint64_t type_hash, WidgetDrawFn fn)
    {
        record_table_[type_hash] = std::move(fn);
    }

    bool WidgetDispatch::draw(const lux::meta::RefField& field,
                              void* component_base,
                              bool* out_edited) const
    {
        // Two-level dispatch: array lookup for primitives, hash map for records.
        const auto base = static_cast<lux::meta::EBaseType>(field.type.qtype.base);
        const WidgetDrawFn* fn_ptr = nullptr;

        if (base != lux::meta::EBaseType::Record  &&
            base != lux::meta::EBaseType::Unknown &&
            base != lux::meta::EBaseType::Void)
        {
            const auto& fn = prim_table_[lux::meta::p_to_underlying(base)];
            if (!fn) return false;
            fn_ptr = &fn;
        }
        else if (base == lux::meta::EBaseType::Record)
        {
            auto it = record_table_.find(field.type.hash);
            if (it == record_table_.end())
            {
                // No widget registered for this record type. Try to recurse
                // into the type's RefClass: if it's in the reflection
                // registry, render each subfield inline under a TreeNode.
                // This is how nested user structs become editable for free
                // as long as their leaves resolve to known widgets.
                return drawRecord(field, component_base, out_edited);
            }
            fn_ptr = &it->second;
        }
        else
        {
            return false;
        }

        // --- label (display_name annotation or raw field name) ---
        auto annot    = field.annotations();
        auto dn       = annot.get("display_name");
        auto label_sv = dn.value_or(field.name);

        char label_buf[128];
        sv_to_cstr(label_sv, label_buf);

        // --- stable ImGui id ("##fieldname") ---
        char id_buf[130];
        id_buf[0] = '#'; id_buf[1] = '#';
        const std::size_t n = std::min(field.name.size(), std::size_t{127});
        std::memcpy(id_buf + 2, field.name.data(), n);
        id_buf[2 + n] = '\0';

        // --- table row ---
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(label_buf);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);

        void* data = static_cast<uint8_t*>(component_base) + field.offset;
        WidgetContext ctx{ id_buf, data, annot };
        const bool edited = (*fn_ptr)(ctx);
        if (out_edited) *out_edited = *out_edited || edited;
        return true;
    }

    // -------------------------------------------------------------------------
    // drawRecord — recursive fallback for unregistered record types
    // -------------------------------------------------------------------------
    bool WidgetDispatch::drawRecord(const lux::meta::RefField& field,
                                    void* component_base,
                                    bool* out_edited) const
    {
        // Look the type up by full name (`field.type.name` is the qualified
        // type name emitted by the reflection generator).
        std::string full_name(field.type.name);
        const lux::meta::RefClass* rc =
            lux::meta::ReflectionRegistry::instance().findClass(full_name);
        if (!rc) return false;

        // --- label row: TreeNode header in col 0, type hint in col 1 ----
        auto annot   = field.annotations();
        auto dn      = annot.get("display_name");
        auto label_sv = dn.value_or(field.name);
        char label_buf[128];
        sv_to_cstr(label_sv, label_buf);

        char id_buf[130];
        id_buf[0] = '#'; id_buf[1] = '#';
        const std::size_t n = std::min(field.name.size(), std::size_t{127});
        std::memcpy(id_buf + 2, field.name.data(), n);
        id_buf[2 + n] = '\0';

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        // Compose a TreeNode label that carries both the display text and a
        // stable per-field id so two fields with the same display name don't
        // collide ImGui-id-wise.
        const auto header = lux::format("{}{}", label_buf, id_buf);
        const bool open = ImGui::TreeNodeEx(header.c_str(),
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("{ %d field%s }",
            static_cast<int>(rc->fields.size()),
            rc->fields.size() == 1 ? "" : "s");

        if (!open) return true;

        // Sub-fields: each rendered as its own row. `field.offset` already
        // applied at the parent dispatch; we're walking inside the same
        // component_base + field.offset slot, so the sub_base is at the
        // same address (the record IS the data at that address).
        void* sub_base = static_cast<uint8_t*>(component_base) + field.offset;
        for (const auto& sub_field : rc->fields)
        {
            if (sub_field.visibility != lux::meta::EVisibility::Public)
                continue;
            // Recurse via the public dispatch — this picks up registered
            // widgets for primitives + supports further nesting.
            draw(sub_field, sub_base, out_edited);
        }

        ImGui::TreePop();
        return true;
    }

    // -------------------------------------------------------------------------
    // Built-in widget registrations
    // -------------------------------------------------------------------------
    void WidgetDispatch::registerBuiltins()
    {
        // ----- float -----
        registerWidget<float>([](const WidgetContext& ctx) {
            auto* v = static_cast<float*>(ctx.data);
            bool edited = false;
            float vmin = 0.f, vmax = 0.f;
            if (annotation_range(ctx.annot, vmin, vmax))
            {
                // Bounded: a finer drag speed than (max-min)*0.005 so editing a
                // wide range (e.g. 0..500) feels continuous, not chunky.
                const float raw   = (vmax - vmin) * 0.001f;
                const float speed = raw > 0.001f ? raw : 0.001f;
                edited = ImGui::DragFloat(ctx.id, v, speed, vmin, vmax, "%.3f");
            }
            else
            {
                // No max (or no annotation) → UNBOUNDED continuous drag (still
                // clamped at min if one is given). Speed scales with magnitude so
                // it stays smooth at any scale — lets e.g. light Range go past any
                // cap. (Replaces the keyboard-only InputFloat that felt discrete.)
                float lo = 0.f;
                const auto sv_min   = ctx.annot.get("min");
                const bool has_min  = sv_min.has_value() && parse_float(*sv_min, lo);
                const float mag     = *v < 0.f ? -*v : *v;
                const float speed   = mag * 0.01f > 0.01f ? mag * 0.01f : 0.01f;
                edited = ImGui::DragFloat(ctx.id, v, speed,
                                 has_min ? lo : 0.f, has_min ? 3.0e38f : 0.f, "%.3f");
            }
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- double -----
        registerWidget<double>([](const WidgetContext& ctx) {
            auto* d = static_cast<double*>(ctx.data);
            float f = static_cast<float>(*d);
            bool edited = false;
            float vmin = 0.f, vmax = 0.f;
            if (annotation_range(ctx.annot, vmin, vmax))
                edited = ImGui::DragFloat(ctx.id, &f, (vmax - vmin) * 0.005f, vmin, vmax, "%.3f");
            else
                edited = ImGui::InputFloat(ctx.id, &f, 0.f, 0.f, "%.4f");
            if (edited)   // write back only on a real edit — the old unconditional
                *d = static_cast<double>(f);   // write truncated doubles every DISPLAY frame
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- int (int32_t / int) -----
        registerWidget<int>([](const WidgetContext& ctx) {
            auto* v = static_cast<int*>(ctx.data);
            bool edited = false;
            float vmin = 0.f, vmax = 0.f;
            if (annotation_range(ctx.annot, vmin, vmax))
                edited = ImGui::DragInt(ctx.id, v, 1.f,
                               static_cast<int>(vmin), static_cast<int>(vmax));
            else
                edited = ImGui::InputInt(ctx.id, v);
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- uint32_t -----
        // Native U32 widget — NEVER round-trip through int. The old int cast +
        // unconditional write-back destroyed any value > INT_MAX just by DISPLAYING
        // it: 0xFFFFFFFF → (int)-1 → clamped to 0 → written back every paint. That
        // silently zeroed Image2DComponent.tint (opaque white → transparent) the
        // moment the Inspector showed a freshly created image — the editor 2D
        // "image invisible / viewport black" root cause. ImGui edits *u in place
        // only on real user interaction; display alone never mutates.
        registerWidget<uint32_t>([](const WidgetContext& ctx) {
            auto* u = static_cast<uint32_t*>(ctx.data);
            bool edited = false;
            float vmin = 0.f, vmax = 0.f;
            if (annotation_range(ctx.annot, vmin, vmax))
            {
                const ImU32 umin = static_cast<ImU32>(vmin < 0.f ? 0.f : vmin);
                const ImU32 umax = static_cast<ImU32>(vmax < 0.f ? 0.f : vmax);
                edited = ImGui::DragScalar(ctx.id, ImGuiDataType_U32, u, 1.f, &umin, &umax);
            }
            else
                edited = ImGui::InputScalar(ctx.id, ImGuiDataType_U32, u);
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- bool -----
        registerWidget<bool>([](const WidgetContext& ctx) {
            auto* b = static_cast<bool*>(ctx.data);
            const bool edited = ImGui::Checkbox(ctx.id, b);
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- Eigen::Vector2f -----
        // (Was missing — 2D component fields (Image2D pivot/size, Transform2D
        // position/scale) fell through to the Inspector's raw-hex fallback.)
        registerWidget<Eigen::Vector2f>([](const WidgetContext& ctx) {
            auto& v      = *static_cast<Eigen::Vector2f*>(ctx.data);
            float arr[2] = {v.x(), v.y()};

            float vmin = 0.f, vmax = 0.f;
            const bool has_range = annotation_range(ctx.annot, vmin, vmax);
            const float speed    = has_range ? (vmax - vmin) * 0.005f : 0.01f;

            // "labels=A,B" overrides the default X/Y axis labels.
            char label_bufs[2][8];
            const char* kLabels[2] = {"X", "Y"};
            auto labels_sv = ctx.annot.get("labels");
            if (labels_sv.has_value() && parse_label_list(*labels_sv, label_bufs, 2) == 2)
                for (int i = 0; i < 2; ++i) kLabels[i] = label_bufs[i];

            bool edited = false;
            if (draw_vec_components(ctx.id, arr, 2, kLabels, speed, vmin, vmax, has_range))
            {
                v      = Eigen::Vector2f(arr[0], arr[1]);
                edited = true;
            }
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- Eigen::Vector3f -----
        registerWidget<Eigen::Vector3f>([](const WidgetContext& ctx) {
            auto& v      = *static_cast<Eigen::Vector3f*>(ctx.data);
            float arr[3] = {v.x(), v.y(), v.z()};
            bool edited  = false;

            // "color=true" → ColorEdit3, otherwise labeled component drag inputs
            auto color_flag = ctx.annot.get("color");
            if (color_flag.has_value() && *color_flag == "true")
            {
                if (ImGui::ColorEdit3(ctx.id, arr))
                {
                    v      = Eigen::Vector3f(arr[0], arr[1], arr[2]);
                    edited = true;
                }
            }
            else
            {
                float vmin = 0.f, vmax = 0.f;
                const bool has_range = annotation_range(ctx.annot, vmin, vmax);
                const float speed    = has_range ? (vmax - vmin) * 0.005f : 0.01f;

                // "labels=A,B,C" overrides the default X/Y/Z axis labels.
                char label_bufs[3][8];
                const char* kLabels[3] = {"X", "Y", "Z"};
                auto labels_sv = ctx.annot.get("labels");
                if (labels_sv.has_value() && parse_label_list(*labels_sv, label_bufs, 3) == 3)
                    for (int i = 0; i < 3; ++i) kLabels[i] = label_bufs[i];

                if (draw_vec_components(ctx.id, arr, 3, kLabels, speed, vmin, vmax, has_range))
                {
                    v      = Eigen::Vector3f(arr[0], arr[1], arr[2]);
                    edited = true;
                }
            }
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- Eigen::Vector4f -----
        registerWidget<Eigen::Vector4f>([](const WidgetContext& ctx) {
            auto& v      = *static_cast<Eigen::Vector4f*>(ctx.data);
            float arr[4] = {v.x(), v.y(), v.z(), v.w()};
            bool edited  = false;

            auto color_flag = ctx.annot.get("color");
            if (color_flag.has_value() && *color_flag == "true")
            {
                if (ImGui::ColorEdit4(ctx.id, arr))
                {
                    v      = Eigen::Vector4f(arr[0], arr[1], arr[2], arr[3]);
                    edited = true;
                }
            }
            else
            {
                float vmin = 0.f, vmax = 0.f;
                const bool has_range = annotation_range(ctx.annot, vmin, vmax);
                const float speed    = has_range ? (vmax - vmin) * 0.005f : 0.01f;

                // "labels=A,B,C,D" overrides the default X/Y/Z/W axis labels.
                char label_bufs[4][8];
                const char* kLabels[4] = {"X", "Y", "Z", "W"};
                auto labels_sv = ctx.annot.get("labels");
                if (labels_sv.has_value() && parse_label_list(*labels_sv, label_bufs, 4) == 4)
                    for (int i = 0; i < 4; ++i) kLabels[i] = label_bufs[i];

                if (draw_vec_components(ctx.id, arr, 4, kLabels, speed, vmin, vmax, has_range))
                {
                    v      = Eigen::Vector4f(arr[0], arr[1], arr[2], arr[3]);
                    edited = true;
                }
            }
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- Eigen::Quaternionf -----
        // Stored as (x,y,z,w); displayed as X/Y/Z/W all clamped to [-1,1].
        registerWidget<Eigen::Quaternionf>([](const WidgetContext& ctx) {
            auto& q      = *static_cast<Eigen::Quaternionf*>(ctx.data);
            float arr[4] = {q.x(), q.y(), q.z(), q.w()};
            static const char* const kLabels[4] = {"X", "Y", "Z", "W"};
            bool edited = false;
            if (draw_vec_components(ctx.id, arr, 4, kLabels, 0.001f, -1.f, 1.f, true))
            {
                q = Eigen::Quaternionf(arr[3], arr[0], arr[1], arr[2]);
                q.normalize();
                edited = true;
            }
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- std::array<float, 8> -----
        // Used by DirectionalLightComponent::cascade_splits. Renders 8
        // InputFloats stacked vertically inside column 1 (we don't have
        // enough horizontal room for an inline DragFloat8). The generator
        // doesn't reflect std::array as a record we can recurse into, so
        // explicit registration is the only path until M2-part-3.
        registerWidget<std::array<float, 8>>([](const WidgetContext& ctx) {
            auto& arr = *static_cast<std::array<float, 8>*>(ctx.data);
            bool edited = false;
            ImGui::PushID(ctx.id);
            for (int i = 0; i < 8; ++i)
            {
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(-FLT_MIN);
                const auto slot_id = lux::format("##{}", i);
                edited = ImGui::InputFloat(slot_id.c_str(), &arr[i], 0.f, 0.f, "%.3f") || edited;
                ImGui::PopID();
            }
            ImGui::PopID();
            maybe_tooltip(ctx.annot);
            return edited;
        });

        // ----- std::string -----
        registerWidget<std::string>([](const WidgetContext& ctx) {
            auto& s = *static_cast<std::string*>(ctx.data);
            char buf[256];
            const std::size_t copy_n = std::min(s.size(), std::size_t{255});
            std::memcpy(buf, s.data(), copy_n);
            buf[copy_n] = '\0';
            bool edited = false;
            if (ImGui::InputText(ctx.id, buf, sizeof(buf)))
            {
                s      = buf;
                edited = true;
            }
            maybe_tooltip(ctx.annot);
            return edited;
        });
    }

} // namespace lux::ui
