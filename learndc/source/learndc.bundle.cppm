export module learndc.bundle;

import std;

extern "C" const unsigned char learndc_bundle_data[];
extern "C" const unsigned long long learndc_bundle_size;

export namespace learndc::bundle
{
    [[nodiscard]] inline std::string_view raw()
    {
        return {reinterpret_cast<char const*>(learndc_bundle_data), static_cast<std::size_t>(learndc_bundle_size)};
    }
}
