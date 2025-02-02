/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Scott Moreau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <wayfire/core.hpp>
#include <wayfire/view.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>


static const char *vertex_shader =
    R"(
#version 100

attribute mediump vec2 position;
attribute mediump vec2 texcoord;

varying mediump vec2 uvpos;

uniform mat4 mvp;

void main() {

   gl_Position = mvp * vec4(position.xy, 0.0, 1.0);
   uvpos = texcoord;
}
)";

static const char *fragment_shader =
    R"(
#version 100
@builtin_ext@
@builtin@

precision mediump float;

uniform mediump vec4 color;
uniform float threshold;

varying mediump vec2 uvpos;

void main()
{
    vec4 c = get_pixel(uvpos);
    vec4 vdiff = abs(vec4(color.r, color.g, color.b, 1.0) - c);
    float diff = max(max(max(vdiff.r, vdiff.g), vdiff.b), vdiff.a);
    if (diff < threshold) {
        c  *= color.a;
        c.a = color.a;
    }
    gl_FragColor = c;
}
)";

static const std::string program_name = "keycolor_shader_program";
static int program_ref_count;

class keycolor_custom_data_t : public wf::custom_data_t
{
  public:
    OpenGL::program_t program;
};

class wf_keycolor : public wf::view_transformer_t
{
    nonstd::observer_ptr<wf::view_interface_t> view;
    wf::config::option_base_t::updated_callback_t option_changed;
    wf::option_wrapper_t<wf::color_t> color{"keycolor/color"};
    wf::option_wrapper_t<double> opacity{"keycolor/opacity"};
    wf::option_wrapper_t<double> threshold{"keycolor/threshold"};

    uint32_t get_z_order() override
    {
        return wf::TRANSFORMER_HIGHLEVEL;
    }

    wf::pointf_t transform_point(
        wf::geometry_t view, wf::pointf_t point) override
    {
        return point;
    }

    wf::pointf_t untransform_point(
        wf::geometry_t view, wf::pointf_t point) override
    {
        return point;
    }

  public:

    wf_keycolor(wayfire_view view) : wf::view_transformer_t()
    {
        this->view = view;

        option_changed = [=] ()
        {
            this->view->damage();
        };

        color.set_callback(option_changed);
        opacity.set_callback(option_changed);
        threshold.set_callback(option_changed);
    }

    void render_with_damage(wf::texture_t src_tex, wlr_box src_box,
        const wf::region_t& damage, const wf::render_target_t& target_fb) override
    {
        wlr_box fb_geom =
            target_fb.framebuffer_box_from_geometry_box(target_fb.geometry);
        auto view_box = target_fb.framebuffer_box_from_geometry_box(src_box);
        view_box.x -= fb_geom.x;
        view_box.y -= fb_geom.y;

        float x = view_box.x, y = view_box.y, w = view_box.width,
            h = view_box.height;

        nonstd::observer_ptr<keycolor_custom_data_t> data =
            wf::get_core().get_data<keycolor_custom_data_t>(program_name);

        static const float vertexData[] = {
            -1.0f, -1.0f,
            1.0f, -1.0f,
            1.0f, 1.0f,
            -1.0f, 1.0f
        };
        static const float texCoords[] = {
            0.0f, 0.0f,
            1.0f, 0.0f,
            1.0f, 1.0f,
            0.0f, 1.0f
        };

        OpenGL::render_begin(target_fb);

        /* Upload data to shader */
        glm::vec4 color_data{
            ((wf::color_t)color).r,
            ((wf::color_t)color).g,
            ((wf::color_t)color).b,
            (double)opacity};
        data->program.use(src_tex.type);
        data->program.uniform4f("color", color_data);
        data->program.uniform1f("threshold", threshold);
        data->program.attrib_pointer("position", 2, 0, vertexData);
        data->program.attrib_pointer("texcoord", 2, 0, texCoords);
        data->program.uniformMatrix4f("mvp", target_fb.transform);
        GL_CALL(glActiveTexture(GL_TEXTURE0));
        data->program.set_active_texture(src_tex);

        /* Render it to target_fb */
        target_fb.bind();
        GL_CALL(glViewport(x, fb_geom.height - y - h, w, h));

        GL_CALL(glEnable(GL_BLEND));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

        for (const auto& box : damage)
        {
            target_fb.logic_scissor(wlr_box_from_pixman_box(box));
            GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
        }

        /* Disable stuff */
        GL_CALL(glDisable(GL_BLEND));
        GL_CALL(glActiveTexture(GL_TEXTURE0));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

        data->program.deactivate();
        OpenGL::render_end();
    }

    virtual ~wf_keycolor()
    {}
};

class wayfire_keycolor : public wf::plugin_interface_t
{
    const std::string transformer_name = "keycolor";

    void add_transformer(wayfire_view view)
    {
        if (view->get_transformer(transformer_name))
        {
            return;
        }

        view->add_transformer(std::make_unique<wf_keycolor>(view),
            transformer_name);
    }

    void pop_transformer(wayfire_view view)
    {
        if (view->get_transformer(transformer_name))
        {
            view->pop_transformer(transformer_name);
        }
    }

    void remove_transformers()
    {
        for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
        {
            pop_transformer(view);
        }
    }

  public:
    void init() override
    {
        grab_interface->name = transformer_name;
        grab_interface->capabilities = 0;

        if (!wf::get_core().get_data<keycolor_custom_data_t>(program_name))
        {
            std::unique_ptr<keycolor_custom_data_t> data =
                std::make_unique<keycolor_custom_data_t>();

            OpenGL::render_begin();
            data->program.compile(vertex_shader, fragment_shader);
            OpenGL::render_end();

            wf::get_core().store_data(std::move(data), program_name);
        }

        program_ref_count++;

        output->connect_signal("view-attached", &view_attached);

        for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
        {
            if (view->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT)
            {
                continue;
            }

            add_transformer(view);
        }
    }

    wf::signal_connection_t view_attached{[this] (wf::signal_data_t *data)
        {
            auto view = get_signaled_view(data);

            if (view->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT)
            {
                return;
            }

            if (!view->get_transformer(transformer_name))
            {
                add_transformer(view);
            }
        }
    };

    void fini() override
    {
        remove_transformers();

        program_ref_count--;

        if (program_ref_count)
        {
            return;
        }

        nonstd::observer_ptr<keycolor_custom_data_t> data =
            wf::get_core().get_data<keycolor_custom_data_t>(program_name);

        OpenGL::render_begin();
        data->program.free_resources();
        OpenGL::render_end();

        wf::get_core().erase_data(program_name);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_keycolor);
