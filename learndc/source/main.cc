import std;
import learndc.bundle;
import learndc.catalog;

int main()
{
    auto source = learndc::bundle::embedded();
    if (!source)
    {
        std::println(std::cerr, "learndc: {}", source.error().message);
        return 3;
    }
    auto loaded = learndc::catalog::Catalog::load(*source);
    if (!loaded)
    {
        std::print(std::cerr, "{}", learndc::catalog::format_errors(loaded.error()));
        return 3;
    }
    std::println("{} exercises", loaded->exercises().size());
    return 0;
}
