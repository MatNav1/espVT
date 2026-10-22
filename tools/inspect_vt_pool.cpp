#include "isobus/isobus/isobus_virtual_terminal_working_set_base.hpp"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
const char *type_name(isobus::VirtualTerminalObjectType type)
{
    using Type = isobus::VirtualTerminalObjectType;
    switch (type)
    {
        case Type::WorkingSet: return "WorkingSet";
        case Type::DataMask: return "DataMask";
        case Type::AlarmMask: return "AlarmMask";
        case Type::Container: return "Container";
        case Type::SoftKeyMask: return "SoftKeyMask";
        case Type::Key: return "Key";
        case Type::Button: return "Button";
        case Type::InputString: return "InputString";
        case Type::InputNumber: return "InputNumber";
        case Type::OutputString: return "OutputString";
        case Type::OutputNumber: return "OutputNumber";
        case Type::OutputLine: return "OutputLine";
        case Type::OutputRectangle: return "OutputRectangle";
        case Type::OutputEllipse: return "OutputEllipse";
        case Type::OutputPolygon: return "OutputPolygon";
        case Type::OutputLinearBarGraph: return "OutputLinearBarGraph";
        case Type::PictureGraphic: return "PictureGraphic";
        case Type::FontAttributes: return "FontAttributes";
        case Type::LineAttributes: return "LineAttributes";
        case Type::FillAttributes: return "FillAttributes";
        case Type::ObjectPointer: return "ObjectPointer";
        default: return "Other";
    }
}
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: inspect_vt_pool pool.iop\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (data.empty())
    {
        std::cerr << "cannot read object pool\n";
        return 2;
    }

    isobus::VirtualTerminalWorkingSetBase working_set;
    if (!working_set.parse_iop_into_objects(data.data(), static_cast<std::uint32_t>(data.size())))
    {
        std::cerr << "parse failed at object " << working_set.get_object_pool_faulting_object_id() << "\n";
        return 1;
    }

    const auto &tree = working_set.get_object_tree();
    std::map<std::uint16_t, std::vector<std::uint16_t>> parents;
    for (const auto &entry : tree)
    {
        for (std::uint16_t index = 0; index < entry.second->get_number_children(); ++index)
        {
            parents[entry.second->get_child_id(index)].push_back(entry.first);
        }
    }

    std::cout << "objects=" << tree.size() << " bytes=" << data.size() << "\n";
    for (const auto &entry : tree)
    {
        const auto &object = entry.second;
        const auto type = object->get_object_type();
        const bool interesting = (type == isobus::VirtualTerminalObjectType::FillAttributes) ||
                                 (type == isobus::VirtualTerminalObjectType::FontAttributes) ||
                                 (type == isobus::VirtualTerminalObjectType::WorkingSet) ||
                                 (type == isobus::VirtualTerminalObjectType::PictureGraphic) ||
                                 (type == isobus::VirtualTerminalObjectType::Button) ||
                                 (type == isobus::VirtualTerminalObjectType::OutputPolygon) ||
                                 (type == isobus::VirtualTerminalObjectType::OutputRectangle) ||
                                 (type == isobus::VirtualTerminalObjectType::OutputLinearBarGraph) ||
                                 (type == isobus::VirtualTerminalObjectType::ObjectPointer);
        if (!interesting)
        {
            continue;
        }

        std::cout << "id=" << entry.first << " type=" << type_name(type)
                  << " size=" << object->get_width() << "x" << object->get_height();
        const auto parent_it = parents.find(entry.first);
        if (parent_it != parents.end())
        {
            std::cout << " parents=";
            for (const auto parent : parent_it->second)
            {
                std::cout << parent << ',';
            }
        }

        if (type == isobus::VirtualTerminalObjectType::FillAttributes)
        {
            const auto fill = std::static_pointer_cast<isobus::FillAttributes>(object);
            std::cout << " fillType=" << static_cast<int>(fill->get_type())
                      << " colour=" << static_cast<int>(fill->get_background_color())
                      << " pattern=" << fill->get_fill_pattern();
        }
        else if (type == isobus::VirtualTerminalObjectType::FontAttributes)
        {
            const auto font = std::static_pointer_cast<isobus::FontAttributes>(object);
            std::cout << " font=" << static_cast<int>(font->get_font_width_pixels()) << 'x'
                      << static_cast<int>(font->get_font_height_pixels())
                      << " colour=" << static_cast<int>(font->get_colour())
                      << " style=0x" << std::hex << static_cast<int>(font->get_style()) << std::dec;
        }
        else if (type == isobus::VirtualTerminalObjectType::OutputPolygon)
        {
            const auto polygon = std::static_pointer_cast<isobus::OutputPolygon>(object);
            std::cout << " lineAttr=" << polygon->get_line_attributes()
                      << " fillAttr=" << polygon->get_fill_attributes()
                      << " polygonType=" << static_cast<int>(polygon->get_type());
        }
        else if (type == isobus::VirtualTerminalObjectType::OutputRectangle)
        {
            const auto rectangle = std::static_pointer_cast<isobus::OutputRectangle>(object);
            std::cout << " lineAttr=" << rectangle->get_line_attributes()
                      << " fillAttr=" << rectangle->get_fill_attributes();
        }
        else if (type == isobus::VirtualTerminalObjectType::ObjectPointer)
        {
            const auto pointer = std::static_pointer_cast<isobus::ObjectPointer>(object);
            std::cout << " value=" << pointer->get_value();
        }
        else if (type == isobus::VirtualTerminalObjectType::PictureGraphic)
        {
            const auto picture = std::static_pointer_cast<isobus::PictureGraphic>(object);
            std::cout << " actual=" << picture->get_actual_width() << 'x' << picture->get_actual_height();
        }
        else if (type == isobus::VirtualTerminalObjectType::Button)
        {
            std::uint32_t options = 0;
            object->get_attribute(static_cast<std::uint8_t>(isobus::Button::AttributeName::Options), options);
            std::cout << " options=0x" << std::hex << options << std::dec;
        }
        std::cout << '\n';
    }

    for (const auto mask_id : { static_cast<std::uint16_t>(1000), static_cast<std::uint16_t>(1001) })
    {
        const auto mask_it = tree.find(mask_id);
        if (mask_it == tree.end())
        {
            continue;
        }
        std::cout << "mask=" << mask_id << " children=" << mask_it->second->get_number_children() << '\n';
        for (std::uint16_t index = 0; index < mask_it->second->get_number_children(); ++index)
        {
            const auto child_id = mask_it->second->get_child_id(index);
            const auto child_it = tree.find(child_id);
            if (child_it == tree.end())
            {
                continue;
            }
            const auto &child = child_it->second;
            std::cout << "  id=" << child_id << " type=" << type_name(child->get_object_type())
                      << " xy=" << mask_it->second->get_child_x(index) << ',' << mask_it->second->get_child_y(index)
                      << " size=" << child->get_width() << 'x' << child->get_height();
            if (child->get_object_type() == isobus::VirtualTerminalObjectType::OutputString)
            {
                const auto output = std::static_pointer_cast<isobus::OutputString>(child);
                std::cout << " fontAttr=" << output->get_font_attributes()
                          << " text=\"" << output->displayed_value(tree) << '\"';
            }
            std::cout << '\n';
        }
    }

    return 0;
}
