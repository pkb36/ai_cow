#pragma once

#include <string>
#include <sstream>

class PipelineBuilder {
public:
    PipelineBuilder& addElement(const std::string& element) {
        if (!elements_.empty()) {
            elements_ += " ! ";
        }
        elements_ += element;
        return *this;
    }

    template<typename... Args>
    PipelineBuilder& addElement(const std::string& format, Args... args) {
        std::stringstream ss;
        addElementImpl(ss, format, args...);
        return addElement(ss.str());
    }

    std::string build() const {
        return elements_;
    }

    void clear() {
        elements_.clear();
    }

private:
    template<typename T, typename... Args>
    void addElementImpl(std::stringstream& ss, const std::string& format, T&& t, Args&&... args) {
        size_t pos = format.find("{}");
        if (pos != std::string::npos) {
            ss << format.substr(0, pos) << t;
            addElementImpl(ss, format.substr(pos + 2), args...);
        } else {
            ss << format;
        }
    }

    void addElementImpl(std::stringstream& ss, const std::string& format) {
        ss << format;
    }

    std::string elements_;
};