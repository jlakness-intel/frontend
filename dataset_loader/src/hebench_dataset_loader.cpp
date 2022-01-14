// Copyright (C) 2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include <cxxabi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <typeinfo>

#include "hebench_dataset_loader.h"

namespace hebench {
namespace DataLoader {

struct icsvstream : std::istringstream
{
    static constexpr const char *s_trim = " \t\n\r\f\v";

    struct csv_ws : std::ctype<char>
    {
        static const mask *make_table()
        {
            static std::vector<mask> v(classic_table(), classic_table() + table_size);
            v[','] |= space;
            //v[' '] &= ~space;
            return &v[0];
        }
        csv_ws(std::size_t refs = 0) :
            ctype(make_table(), false, refs) {}
    };
    static std::locale loc;
    template <class... Args>
    icsvstream(Args &&... args) :
        std::istringstream(std::forward<Args>(args)...)
    {
        exceptions(std::ifstream::failbit);
        imbue(loc);
    }
};

std::locale icsvstream::loc = std::locale(std::locale(), new icsvstream::csv_ws());

template <typename T>
// returns actual number of lines read
std::size_t loadcsvdatafile(std::ifstream &ifs, std::vector<std::vector<T>> &v, size_t nlines, size_t skip = 0, size_t lnum = 0, std::string fpath = "")
{
    std::size_t retval = 0;
    std::string line;
    while (skip--)
    {
        ++lnum;
        std::getline(ifs, line);
    }
    while (nlines-- and std::getline(ifs, line))
    {
        ++retval;
        ++lnum;
        line.erase(0, line.find_first_not_of(icsvstream::s_trim));
        line.erase(line.find_last_not_of(icsvstream::s_trim) + 1);
        if (line.size() == 0 or line.at(0) == '#')
        {
            ++nlines;
            continue;
        }
        icsvstream ils(line);
        std::vector<T> w;
        std::istream_iterator<T> isit(ils);
        try
        {
            std::copy(isit, std::istream_iterator<T>(), std::back_inserter(w));
        }
        catch (const std::ios_base::failure &e)
        {
            if (!ils.eof())
            {
                //throw; // preserves error type and attributes
                std::ostringstream oss;
                size_t ofs = ils.rdbuf()->pubseekoff(0, std::ios_base::cur, std::ios_base::in);
                int status;
                oss << e.what() << " in data type \"" << abi::__cxa_demangle(typeid(T).name(), 0, 0, &status) << "\""
                    << " at " << fpath << ":" << lnum << ":" << ofs << std::endl;
                throw std::ios_base::failure(oss.str(), e.code());
            }
        }
        if (v.size() and w.size() != v.back().size())
        {
            std::ostringstream ss;
            ss << "Inconsistent number of values read from line " << lnum << ". Previous: " << v.back().size() << ". This: " << w.size();
            throw std::length_error(ss.str());
        }
        v.push_back(w);
    }
    if ((nlines + 1) != 0)
    {
        throw(std::length_error("Not enought lines read before end of file."));
    }

    return retval;
}

template <typename T>
icsvstream &operator>>(icsvstream &in, T &arg)
{
    std::istringstream *iss = &in;
    try
    {
        (*iss) >> arg; // superclass read data into argument
    }
    catch (const std::ios_base::failure &e)
    {
        std::ostringstream oss;
        int status;
        oss << e.what() << " in data type \"" << abi::__cxa_demangle(typeid(T).name(), 0, 0, &status) << "\"";
        throw std::ios_base::failure(oss.str(), e.code());
    }
    return in;
}

template <typename T, typename E>
ExternalDataset<T> ExternalDatasetLoader<T, E>::loadFromCSV(const std::string &filename, std::uint64_t max_loaded_size)
{
    size_t lnum = 0;
    ExternalDataset<T> eds;
    std::ifstream ifs(filename, std::ifstream::in);
    ifs.exceptions(std::ifstream::badbit);
    std::string metaline;
    while (std::getline(ifs, metaline))
    {
        ++lnum;
        metaline.erase(0, metaline.find_first_not_of(icsvstream::s_trim));
        metaline.erase(metaline.find_last_not_of(icsvstream::s_trim) + 1);
        if (metaline.size() == 0 or metaline.at(0) == '#')
            continue;
        icsvstream iss(metaline);
        std::string tag, kind;
        size_t ix, nlines;
        iss >> tag >> ix >> nlines >> kind;
        std::vector<std::vector<std::vector<T>>> *u;
        if (tag == "input")
            u = &eds.inputs;
        else if (tag == "output")
            u = &eds.outputs;
        else
        {
            std::stringstream ss;
            ss << "Parse error: <tag> must be \"input\" or \"output\". Got: " << tag << " at "
               << filename << ":" << lnum;
            throw std::runtime_error(ss.str());
        }
        if (ix >= u->size())
            u->resize(ix + 1);
        std::vector<std::vector<T>> &v = u->at(ix);
        if (kind == "csv")
        {
            std::string dataline;
            while (nlines > 0 && std::getline(ifs, dataline))
            {
                ++lnum;
                dataline.erase(0, dataline.find_first_not_of(icsvstream::s_trim));
                dataline.erase(dataline.find_last_not_of(icsvstream::s_trim) + 1);
                if (dataline.empty() || dataline.front() == '#')
                    continue;
                icsvstream ils(dataline);
                std::string fname;
                size_t from_line = 1, num_lines = 0;
                ils >> fname >> from_line >> num_lines;
                std::filesystem::path path = fname;
                if (path.is_relative())
                    path = std::filesystem::path(filename).remove_filename() / path;
                path = std::filesystem::canonical(path);
                std::ifstream ifs_csv(path, std::ifstream::in);
                ifs_csv.exceptions(std::ifstream::badbit);
                loadcsvdatafile(ifs_csv, v, num_lines, from_line - 1, 0, path);
                ifs_csv.close();
                --nlines;
            }
        }
        else if (kind == "local")
        {
            lnum += loadcsvdatafile(ifs, v, nlines, 0, lnum, filename);
        }
        else
        {
            std::stringstream ss;
            ss << "Parse error: <kind> must be \"local\" or \"csv\". Got: " << kind << " at "
               << filename << ":" << lnum;
            throw std::runtime_error(ss.str());
        }
    }
    if (eds.outputs.size())
    {
        size_t n = 1;
        for (auto &v : eds.inputs)
            n *= v.size();
        if (eds.outputs[0].size() != n)
            throw std::length_error("Output size (" + std::to_string(eds.outputs[0].size()) + ") must be the product of the input sizes (" + std::to_string(n) + ").");
    }
    ifs.close();

    // make sure we did not go above permited max size
    if (max_loaded_size > 0)
    {
        std::uint64_t loaded_size = 0;
        for (const auto &input_component : eds.inputs)
            for (const auto &input_sample : input_component)
                loaded_size += (input_sample.size() * sizeof(T));
        for (const auto &output_component : eds.outputs)
            for (const auto &output_sample : output_component)
                loaded_size += (output_sample.size() * sizeof(T));
        if (loaded_size > max_loaded_size)
            throw std::runtime_error("Loaded size greater than maximum permitted. Maximum: "
                                     + std::to_string(max_loaded_size) + " bytes; loaded: "
                                     + std::to_string(loaded_size) + " bytes.");
    }

    return eds;
}

} // namespace DataLoader
} // namespace hebench
