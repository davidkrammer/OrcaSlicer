#include "../libslic3r.h"
#include "../Model.hpp"
#include "../PNGReadWrite.hpp"
#include "../TriangleMesh.hpp"

#include "OBJ.hpp"
#include "objparser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

//Translation
#include "I18N.hpp"
#define _L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

namespace {

static std::string resolve_obj_asset_path(const char *obj_path, const std::string &asset_path)
{
    boost::filesystem::path texture_path(asset_path);
    if (texture_path.is_absolute())
        return texture_path.string();

    boost::filesystem::path full_obj_path(obj_path);
    return (full_obj_path.parent_path() / texture_path).string();
}

static std::string lower_extension(boost::filesystem::path path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

static bool extension_matches(const boost::filesystem::path &path, const std::vector<std::string> &extensions)
{
    const std::string ext = lower_extension(path);
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

static boost::filesystem::path fallback_obj_asset_path(const char *obj_path, const boost::filesystem::path &requested_path, const std::vector<std::string> &extensions)
{
    const boost::filesystem::path full_obj_path(obj_path);
    const boost::filesystem::path dir = full_obj_path.parent_path();
    if (dir.empty() || !boost::filesystem::exists(dir))
        return {};

    if (!requested_path.extension().empty()) {
        boost::filesystem::path same_stem = dir / full_obj_path.stem();
        same_stem.replace_extension(requested_path.extension());
        if (boost::filesystem::exists(same_stem))
            return same_stem;
    }

    boost::filesystem::path only_match;
    size_t match_count = 0;
    for (const boost::filesystem::directory_entry &entry : boost::filesystem::directory_iterator(dir)) {
        if (!boost::filesystem::is_regular_file(entry.status()) || !extension_matches(entry.path(), extensions))
            continue;
        only_match = entry.path();
        ++match_count;
        if (match_count > 1)
            return {};
    }

    return match_count == 1 ? only_match : boost::filesystem::path{};
}

static std::string resolve_existing_obj_asset_path(const char *obj_path, const std::string &asset_path, const std::vector<std::string> &fallback_extensions)
{
    const boost::filesystem::path resolved_path(resolve_obj_asset_path(obj_path, asset_path));
    if (boost::filesystem::exists(resolved_path))
        return resolved_path.string();

    const boost::filesystem::path fallback_path = fallback_obj_asset_path(obj_path, resolved_path, fallback_extensions);
    return fallback_path.empty() ? resolved_path.string() : fallback_path.string();
}

static bool load_png_texture(const std::string &texture_path, png::ImageColorscale &image)
{
    if (!boost::filesystem::exists(texture_path))
        return false;

    std::ifstream texture_file(texture_path, std::ios::binary);
    if (!texture_file)
        return false;

    std::vector<char> buffer{std::istreambuf_iterator<char>(texture_file), std::istreambuf_iterator<char>()};
    if (buffer.empty())
        return false;

    png::ReadBuf read_buffer{buffer.data(), buffer.size()};
    return png::decode_colored_png(read_buffer, image);
}

static float wrap_uv(float value)
{
    value -= std::floor(value);
    return value < 0.f ? value + 1.f : value;
}

static RGBA sample_texture(const png::ImageColorscale &image, float u, float v)
{
    if (image.cols == 0 || image.rows == 0 || image.buf.empty())
        return RGBA{1.f, 1.f, 1.f, 1.f};

    const size_t x = std::min<size_t>(image.cols - 1, static_cast<size_t>(std::lround(wrap_uv(u) * float(image.cols - 1))));
    const size_t y = std::min<size_t>(image.rows - 1, static_cast<size_t>(std::lround(wrap_uv(v) * float(image.rows - 1))));
    const size_t bytes_per_pixel = static_cast<size_t>(image.bytes_per_pixel);
    const size_t offset = (y * image.cols + x) * bytes_per_pixel;

    if (offset + 2 >= image.buf.size())
        return RGBA{1.f, 1.f, 1.f, 1.f};

    return RGBA{
        float(image.buf[offset]) / 255.f,
        float(image.buf[offset + 1]) / 255.f,
        float(image.buf[offset + 2]) / 255.f,
        bytes_per_pixel >= 4 && offset + 3 < image.buf.size() ? float(image.buf[offset + 3]) / 255.f : 1.f
    };
}

} // namespace

bool load_obj(const char *path, TriangleMesh *meshptr, ObjInfo& obj_info, std::string &message)
{
    if (meshptr == nullptr)
        return false;
    // Parse the OBJ file.
    ObjParser::ObjData data;
    ObjParser::MtlData mtl_data;
    if (! ObjParser::objparse(path, data)) {
        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path;
        message = _L("load_obj: failed to parse");
        return false;
    }
    bool exist_mtl = false;
    if (data.mtllibs.size() > 0) { // read mtl
        for (auto mtl_name : data.mtllibs) {
            if (mtl_name.size() == 0){
                continue;
            }
            exist_mtl = true;
            bool                    mtl_name_is_path = false;
            boost::filesystem::path mtl_abs_path(mtl_name);
            if (boost::filesystem::exists(mtl_abs_path)) {
                mtl_name_is_path = true;
            }
            boost::filesystem::path mtl_path;
            if (!mtl_name_is_path) {
                boost::filesystem::path full_path(path);
                std::string             dir = full_path.parent_path().string();
                auto                    mtl_file = dir + "/" + mtl_name;
                boost::filesystem::path temp_mtl_path(mtl_file);
                mtl_path = temp_mtl_path;
            }
            boost::filesystem::path resolved_mtl_path = mtl_name_is_path ? mtl_abs_path : mtl_path;
            if (!boost::filesystem::exists(resolved_mtl_path)) {
                const boost::filesystem::path fallback_mtl_path = fallback_obj_asset_path(path, resolved_mtl_path, {".mtl"});
                if (!fallback_mtl_path.empty()) {
                    BOOST_LOG_TRIVIAL(info) << "load_obj: using fallback mtl_path:" << fallback_mtl_path.string();
                    resolved_mtl_path = fallback_mtl_path;
                }
            }

            if (boost::filesystem::exists(resolved_mtl_path)) {
                const std::string resolved_mtl_path_string = resolved_mtl_path.string();
                if (!ObjParser::mtlparse(resolved_mtl_path_string.c_str(), mtl_data)) {
                    BOOST_LOG_TRIVIAL(error) << "load_obj:load_mtl: failed to parse " << resolved_mtl_path_string;
                    message = _L("load mtl in obj: failed to parse");
                    return false;
                }
            }
            else {
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to load mtl_path:" << resolved_mtl_path.string();
            }
        }
    }
    // Count the faces and verify, that all faces are triangular.
    size_t num_faces = 0;
    size_t num_quads = 0;
    for (size_t i = 0; i < data.vertices.size(); ++ i) {
        // Find the end of face.
        size_t j = i;
        for (; j < data.vertices.size() && data.vertices[j].coordIdx != -1; ++ j) ;
        if (size_t num_face_vertices = j - i; num_face_vertices > 0) {
            if (num_face_vertices > 4) {
                // Non-triangular and non-quad faces are not supported as of now.
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains polygons with more than 4 vertices.";
                message = _L("The file contains polygons with more than 4 vertices.");
                return false;
            } else if (num_face_vertices < 3) {
                // Non-triangular and non-quad faces are not supported as of now.
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains polygons with less than 2 vertices.";
                message = _L("The file contains polygons with less than 2 vertices.");
                return false;
            }
            if (num_face_vertices == 4)
                ++ num_quads;
            ++ num_faces;
            i = j;
        }
    }
    // Convert ObjData into indexed triangle set.
    indexed_triangle_set its;
    size_t               num_vertices = data.coordinates.size() / OBJ_VERTEX_LENGTH;
    its.vertices.reserve(num_vertices);
    its.indices.reserve(num_faces + num_quads);
    if (exist_mtl) {
        obj_info.face_colors.reserve(num_faces + num_quads);
    }
    bool has_color = data.has_vertex_color;
    for (size_t i = 0; i < num_vertices; ++ i) {
        size_t j = i * OBJ_VERTEX_LENGTH;
        its.vertices.emplace_back(data.coordinates[j], data.coordinates[j + 1], data.coordinates[j + 2]);
        if (data.has_vertex_color) {
            RGBA color{std::clamp(data.coordinates[j + 3], 0.f, 1.f), std::clamp(data.coordinates[j + 4], 0.f, 1.f), std::clamp(data.coordinates[j + 5], 0.f, 1.f),
                       std::clamp(data.coordinates[j + 6], 0.f, 1.f)};
            obj_info.vertex_colors.emplace_back(color);
        }
    }
    int indices[ONE_FACE_SIZE];
    int uvs[ONE_FACE_SIZE];
    std::unordered_map<std::string, png::ImageColorscale> texture_cache;
    std::unordered_set<std::string> failed_texture_cache;
    for (size_t i = 0; i < data.vertices.size();)
        if (data.vertices[i].coordIdx == -1)
            ++ i;
        else {
            int cnt = 0;
            while (i < data.vertices.size())
                if (const ObjParser::ObjVertex &vertex = data.vertices[i ++]; vertex.coordIdx == -1) {
                    break;
                } else {
                    assert(cnt < OBJ_VERTEX_LENGTH);
                    if (vertex.coordIdx < 0 || vertex.coordIdx >= int(its.vertices.size())) {
                        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains invalid vertex index.";
                        message = _L("The file contains invalid vertex index.");
                        return false;
                    }
                    indices[cnt] = vertex.coordIdx;
                    uvs[cnt]     = vertex.textureCoordIdx;
                    cnt++;
                }
            if (cnt) {
                assert(cnt == 3 || cnt == 4);
                // Insert one or two faces (triangulate a quad).
                its.indices.emplace_back(indices[0], indices[1], indices[2]);
                int  face_index =its.indices.size() - 1;
                RGBA face_color;
                auto sample_face_texture = [&data, &texture_cache, &failed_texture_cache, path](const std::string &texture_name, const std::array<int, 3> &face_uvs, RGBA &out_color) {
                    if (texture_name.empty() || data.textureCoordinates.empty())
                        return false;

                    for (int uv_index : face_uvs)
                        if (uv_index < 0 || size_t(uv_index * 2 + 1) >= data.textureCoordinates.size())
                            return false;

                    const std::string texture_path = resolve_existing_obj_asset_path(path, texture_name, {".png", ".jpg", ".jpeg"});
                    if (failed_texture_cache.find(texture_path) != failed_texture_cache.end())
                        return false;

                    auto texture_it = texture_cache.find(texture_path);
                    if (texture_it == texture_cache.end()) {
                        png::ImageColorscale image;
                        if (!load_png_texture(texture_path, image)) {
                            failed_texture_cache.insert(texture_path);
                            return false;
                        }
                        texture_it = texture_cache.emplace(texture_path, std::move(image)).first;
                    }

                    const float u0 = data.textureCoordinates[face_uvs[0] * 2];
                    const float v0 = data.textureCoordinates[face_uvs[0] * 2 + 1];
                    const float u1 = data.textureCoordinates[face_uvs[1] * 2];
                    const float v1 = data.textureCoordinates[face_uvs[1] * 2 + 1];
                    const float u2 = data.textureCoordinates[face_uvs[2] * 2];
                    const float v2 = data.textureCoordinates[face_uvs[2] * 2 + 1];

                    out_color = sample_texture(texture_it->second, (u0 + u1 + u2) / 3.f, (v0 + v1 + v2) / 3.f);
                    return true;
                };
                auto set_face_color = [&data, &mtl_data, &obj_info, &face_color, &sample_face_texture](int face_index, const std::string mtl_name, const std::array<int, 3> face_uvs) {
                    if (mtl_data.new_mtl_unmap.find(mtl_name) != mtl_data.new_mtl_unmap.end()) {
                        const auto &material = mtl_data.new_mtl_unmap[mtl_name];
                        bool is_merge_ka_kd = true;
                        for (size_t n = 0; n < 3; n++) {
                            if (float(material->Ka[n] + material->Kd[n]) > 1.0) {
                                is_merge_ka_kd=false;
                                break;
                            }
                        }
                        for (size_t n = 0; n < 3; n++) {
                            if (is_merge_ka_kd) {
                                face_color[n] = std::clamp(float(material->Ka[n] + material->Kd[n]), 0.f, 1.f);
                            }
                            else {
                                face_color[n] = std::clamp(float(material->Kd[n]), 0.f, 1.f);
                            }
                        }
                        face_color[3] = material->Tr; // alpha
                        if (material->map_Kd.size() > 0) {
                            auto png_name       = material->map_Kd;
                            obj_info.has_uv_png = true;
                            if (obj_info.pngs.find(png_name) == obj_info.pngs.end()) { obj_info.pngs[png_name] = false; }
                            obj_info.uv_map_pngs[face_index] = png_name;
                            sample_face_texture(png_name, face_uvs, face_color);
                        }
                        if (data.textureCoordinates.size() > 0) {
                            bool valid_uvs = true;
                            for (int uv_index : face_uvs)
                                if (uv_index < 0 || size_t(uv_index * 2 + 1) >= data.textureCoordinates.size())
                                    valid_uvs = false;
                            if (valid_uvs) {
                                Vec2f                uv0(data.textureCoordinates[face_uvs[0] * 2], data.textureCoordinates[face_uvs[0] * 2 + 1]);
                                Vec2f                uv1(data.textureCoordinates[face_uvs[1] * 2], data.textureCoordinates[face_uvs[1] * 2 + 1]);
                                Vec2f                uv2(data.textureCoordinates[face_uvs[2] * 2], data.textureCoordinates[face_uvs[2] * 2 + 1]);
                                std::array<Vec2f, 3> uv_array{uv0, uv1, uv2};
                                obj_info.uvs.emplace_back(uv_array);
                            }
                        }
                        obj_info.face_colors.emplace_back(face_color);
                    }
                };
                auto set_face_color_by_mtl = [&data, &set_face_color](int face_index, const std::array<int, 3> face_uvs) {
                    if (data.usemtls.size() == 1) {
                        set_face_color(face_index, data.usemtls[0].name, face_uvs);
                    } else {
                        for (size_t k = 0; k < data.usemtls.size(); k++) {
                            auto mtl = data.usemtls[k];
                            if (face_index >= mtl.face_start && face_index <= mtl.face_end) {
                                set_face_color(face_index, data.usemtls[k].name, face_uvs);
                                break;
                            }
                        }
                    }
                };
                if (exist_mtl) {
                    set_face_color_by_mtl(face_index, {uvs[0], uvs[1], uvs[2]});
                }
                if (cnt == 4) {
                    its.indices.emplace_back(indices[0], indices[2], indices[3]);
                    int face_index = its.indices.size() - 1;
                    if (exist_mtl) {
                        set_face_color_by_mtl(face_index, {uvs[0], uvs[2], uvs[3]});
                    }
                }
            }
        }

    if (!obj_info.face_colors.empty()) {
        const RGBA first_color = obj_info.face_colors.front();
        obj_info.is_single_mtl = std::all_of(obj_info.face_colors.begin() + 1, obj_info.face_colors.end(), [&first_color](const RGBA &color) {
            return color_is_equal(first_color, color);
        });
    }

    *meshptr = TriangleMesh(std::move(its));
    if (meshptr->empty()) {
        BOOST_LOG_TRIVIAL(error) << "load_obj: This OBJ file couldn't be read because it's empty. " << path;
        message = _L("This OBJ file couldn't be read because it's empty.");
        return false;
    }
    if (meshptr->volume() < 0)
        meshptr->flip_triangles();
    return true;
}

bool load_obj(const char *path, Model *model, ObjInfo& obj_info, std::string &message, const char *object_name_in)
{
    TriangleMesh mesh;

    bool ret = load_obj(path, &mesh, obj_info, message);

    if (ret) {
        std::string  object_name;
        if (object_name_in == nullptr) {
            const char *last_slash = strrchr(path, DIR_SEPARATOR);
            object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
        } else
           object_name.assign(object_name_in);
        model->add_object(object_name.c_str(), path, std::move(mesh));
    }

    return ret;
}

bool store_obj(const char *path, TriangleMesh *mesh)
{
    //FIXME returning false even if write failed.
    mesh->WriteOBJFile(path);
    return true;
}

bool store_obj(const char *path, ModelObject *model_object)
{
    TriangleMesh mesh = model_object->mesh();
    return store_obj(path, &mesh);
}

bool store_obj(const char *path, Model *model)
{
    TriangleMesh mesh = model->mesh();
    return store_obj(path, &mesh);
}

}; // namespace Slic3r
