#include "metrics/metric_series.h"

namespace metrics {

// Explicit template instantiations
template class MetricSeries<double>;
template class MetricSeries<float>;

} // namespace metrics
