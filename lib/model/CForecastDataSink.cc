/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License;
 * you may not use this file except in compliance with the Elastic License.
 */

#include <model/CForecastDataSink.h>

#include <core/CLogger.h>
#include <core/CScopedRapidJsonPoolAllocator.h>

#include <vector>

namespace ml {
namespace model {

namespace {
using TStrVec = std::vector<std::string>;

// static strings
const std::string STATUS_SCHEDULED("scheduled");
const std::string STATUS_STARTED("started");
const std::string STATUS_FINISHED("finished");
const std::string STATUS_FAILED("failed");

} // unnamed

// JSON field names
const std::string CForecastDataSink::JOB_ID("job_id");
const std::string CForecastDataSink::DETECTOR_INDEX("detector_index");
const std::string CForecastDataSink::FORECAST_ID("forecast_id");
const std::string CForecastDataSink::FORECAST_ALIAS("forecast_alias");
const std::string CForecastDataSink::MODEL_FORECAST("model_forecast");
const std::string CForecastDataSink::MODEL_FORECAST_STATS("model_forecast_request_stats");
const std::string CForecastDataSink::PARTITION_FIELD_NAME("partition_field_name");
const std::string CForecastDataSink::PARTITION_FIELD_VALUE("partition_field_value");
const std::string CForecastDataSink::FEATURE("model_feature");
const std::string CForecastDataSink::BY_FIELD_NAME("by_field_name");
const std::string CForecastDataSink::BY_FIELD_VALUE("by_field_value");
const std::string CForecastDataSink::LOWER("forecast_lower");
const std::string CForecastDataSink::UPPER("forecast_upper");
const std::string CForecastDataSink::PREDICTION("forecast_prediction");
const std::string CForecastDataSink::BUCKET_SPAN("bucket_span");
const std::string CForecastDataSink::PROCESSED_RECORD_COUNT("processed_record_count");
const std::string CForecastDataSink::CREATE_TIME("forecast_create_timestamp");
const std::string CForecastDataSink::TIMESTAMP("timestamp");
const std::string CForecastDataSink::START_TIME("forecast_start_timestamp");
const std::string CForecastDataSink::END_TIME("forecast_end_timestamp");
const std::string CForecastDataSink::EXPIRY_TIME("forecast_expiry_timestamp");
const std::string CForecastDataSink::MEMORY_USAGE("forecast_memory_bytes");
const std::string CForecastDataSink::MESSAGES("forecast_messages");
const std::string CForecastDataSink::PROCESSING_TIME_MS("processing_time_ms");
const std::string CForecastDataSink::PROGRESS("forecast_progress");
const std::string CForecastDataSink::STATUS("forecast_status");

using TScopedAllocator = core::CScopedRapidJsonPoolAllocator<core::CRapidJsonConcurrentLineWriter>;

CForecastDataSink::SForecastModelWrapper::SForecastModelWrapper(model_t::EFeature feature,
                                                                TMathsModelPtr&& forecastModel,
                                                                const std::string& byFieldValue)
    : s_Feature(feature), s_ForecastModel(std::move(forecastModel)),
      s_ByFieldValue(byFieldValue) {
}

CForecastDataSink::SForecastModelWrapper::SForecastModelWrapper(SForecastModelWrapper&& other)
    : s_Feature(other.s_Feature), s_ForecastModel(std::move(other.s_ForecastModel)),
      s_ByFieldValue(std::move(other.s_ByFieldValue)) {
}

CForecastDataSink::SForecastResultSeries::SForecastResultSeries(const SModelParams& modelParams)
    : s_ModelParams(modelParams), s_DetectorIndex(), s_ToForecastPersisted(),
      s_ByFieldName(), s_MinimumSeasonalVarianceScale(0.0) {
}

CForecastDataSink::SForecastResultSeries::SForecastResultSeries(SForecastResultSeries&& other)
    : s_ModelParams(std::move(other.s_ModelParams)),
      s_DetectorIndex(other.s_DetectorIndex),
      s_ToForecast(std::move(other.s_ToForecast)),
      s_ToForecastPersisted(std::move(other.s_ToForecastPersisted)),
      s_PartitionFieldName(std::move(other.s_PartitionFieldName)),
      s_PartitionFieldValue(std::move(other.s_PartitionFieldValue)),
      s_ByFieldName(std::move(other.s_ByFieldName)),
      s_MinimumSeasonalVarianceScale(other.s_MinimumSeasonalVarianceScale) {
}

CForecastDataSink::CForecastDataSink(const std::string& jobId,
                                     const std::string& forecastId,
                                     const std::string& forecastAlias,
                                     core_t::TTime createTime,
                                     core_t::TTime startTime,
                                     core_t::TTime endTime,
                                     core_t::TTime expiryTime,
                                     size_t memoryUsage,
                                     core::CJsonOutputStreamWrapper& outStream)
    : m_JobId(jobId), m_ForecastId(forecastId), m_ForecastAlias(forecastAlias),
      m_Writer(outStream), m_NumRecordsWritten(0), m_CreateTime(createTime),
      m_StartTime(startTime), m_EndTime(endTime), m_ExpiryTime(expiryTime),
      m_MemoryUsage(memoryUsage) {
}

void CForecastDataSink::writeStats(const double progress,
                                   uint64_t runtime,
                                   const TStrUMap& messages,
                                   bool successful) {
    TScopedAllocator scopedAllocator("CForecastDataSink", m_Writer);

    rapidjson::Document doc = m_Writer.makeDoc();

    this->writeCommonStatsFields(doc);
    m_Writer.addUIntFieldToObj(MEMORY_USAGE, m_MemoryUsage, doc);

    m_Writer.addUIntFieldToObj(PROCESSED_RECORD_COUNT, m_NumRecordsWritten, doc);
    m_Writer.addDoubleFieldToObj(PROGRESS, progress, doc);
    m_Writer.addUIntFieldToObj(PROCESSING_TIME_MS, runtime, doc);

    m_Writer.addStringArrayFieldToObj(MESSAGES, messages, doc);
    if (progress < 1.0) {
        m_Writer.addStringFieldReferenceToObj(STATUS, STATUS_STARTED, doc);
    } else {
        if (successful) {
            m_Writer.addStringFieldReferenceToObj(STATUS, STATUS_FINISHED, doc);
        } else {
            m_Writer.addStringFieldReferenceToObj(STATUS, STATUS_FAILED, doc);
        }
    }

    // only flush after the last record
    this->push(progress == 1.0, doc);
}

void CForecastDataSink::writeScheduledMessage() {
    rapidjson::Value doc(rapidjson::kObjectType);
    this->writeCommonStatsFields(doc);
    m_Writer.addStringFieldReferenceToObj(STATUS, STATUS_SCHEDULED, doc);
    this->push(true /*important, therefore flush*/, doc);
}

void CForecastDataSink::writeErrorMessage(const std::string& message) {
    TScopedAllocator scopedAllocator("CForecastDataSink", m_Writer);

    rapidjson::Document doc = m_Writer.makeDoc();
    this->writeCommonStatsFields(doc);
    TStrVec messages{message};
    m_Writer.addStringArrayFieldToObj(MESSAGES, messages, doc);
    m_Writer.addStringFieldReferenceToObj(STATUS, STATUS_FAILED, doc);
    this->push(true /*important, therefore flush*/, doc);
}

void CForecastDataSink::writeFinalMessage(const std::string& message) {
    TScopedAllocator scopedAllocator("CForecastDataSink", m_Writer);

    rapidjson::Document doc = m_Writer.makeDoc();
    this->writeCommonStatsFields(doc);
    TStrVec messages{message};
    m_Writer.addStringArrayFieldToObj(MESSAGES, messages, doc);
    m_Writer.addDoubleFieldToObj(PROGRESS, 1.0, doc);
    m_Writer.addStringFieldReferenceToObj(STATUS, STATUS_FINISHED, doc);
    this->push(true /*important, therefore flush*/, doc);
}

void CForecastDataSink::writeCommonStatsFields(rapidjson::Value& doc) {
    m_Writer.addStringFieldReferenceToObj(JOB_ID, m_JobId, doc);
    m_Writer.addStringFieldReferenceToObj(FORECAST_ID, m_ForecastId, doc);
    if (m_ForecastAlias.empty() == false) {
        m_Writer.addStringFieldReferenceToObj(FORECAST_ALIAS, m_ForecastAlias, doc);
    }
    m_Writer.addTimeFieldToObj(CREATE_TIME, m_CreateTime, doc);
    m_Writer.addTimeFieldToObj(TIMESTAMP, m_StartTime, doc);
    m_Writer.addTimeFieldToObj(START_TIME, m_StartTime, doc);
    m_Writer.addTimeFieldToObj(END_TIME, m_EndTime, doc);

    if (m_ExpiryTime != m_CreateTime) {
        m_Writer.addTimeFieldToObj(EXPIRY_TIME, m_ExpiryTime, doc);
    }
}

void CForecastDataSink::push(bool flush, rapidjson::Value& doc) {
    TScopedAllocator scopedAllocator("CForecastDataSink", m_Writer);

    rapidjson::Document wrapper = m_Writer.makeDoc();

    m_Writer.addMember(MODEL_FORECAST_STATS, doc, wrapper);
    m_Writer.write(wrapper);

    if (flush) {
        m_Writer.flush();
    }
}

uint64_t CForecastDataSink::numRecordsWritten() const {
    return m_NumRecordsWritten;
}

void CForecastDataSink::push(const maths::SErrorBar errorBar,
                             const std::string& feature,
                             const std::string& partitionFieldName,
                             const std::string& partitionFieldValue,
                             const std::string& byFieldName,
                             const std::string& byFieldValue,
                             int detectorIndex) {
    TScopedAllocator scopedAllocator("CForecastDataSink", m_Writer);

    ++m_NumRecordsWritten;
    rapidjson::Document doc = m_Writer.makeDoc();

    m_Writer.addStringFieldReferenceToObj(JOB_ID, m_JobId, doc);
    m_Writer.addIntFieldToObj(DETECTOR_INDEX, detectorIndex, doc);
    m_Writer.addStringFieldReferenceToObj(FORECAST_ID, m_ForecastId, doc);
    if (m_ForecastAlias.empty() == false) {
        m_Writer.addStringFieldReferenceToObj(FORECAST_ALIAS, m_ForecastAlias, doc);
    }
    m_Writer.addStringFieldCopyToObj(FEATURE, feature, doc, true);
    // time is in Java format - milliseconds since the epoch
    m_Writer.addTimeFieldToObj(TIMESTAMP, errorBar.s_Time, doc);
    m_Writer.addIntFieldToObj(BUCKET_SPAN, errorBar.s_BucketLength, doc);
    if (!partitionFieldName.empty()) {
        m_Writer.addStringFieldCopyToObj(PARTITION_FIELD_NAME, partitionFieldName, doc);
        m_Writer.addStringFieldCopyToObj(PARTITION_FIELD_VALUE,
                                         partitionFieldValue, doc, true);
    }
    if (!byFieldName.empty()) {
        m_Writer.addStringFieldCopyToObj(BY_FIELD_NAME, byFieldName, doc);
        m_Writer.addStringFieldCopyToObj(BY_FIELD_VALUE, byFieldValue, doc, true);
    }

    m_Writer.addDoubleFieldToObj(LOWER, errorBar.s_LowerBound, doc);
    m_Writer.addDoubleFieldToObj(UPPER, errorBar.s_UpperBound, doc);
    m_Writer.addDoubleFieldToObj(PREDICTION, errorBar.s_Predicted, doc);

    rapidjson::Document wrapper = m_Writer.makeDoc();
    m_Writer.addMember(MODEL_FORECAST, doc, wrapper);
    m_Writer.write(wrapper);
}

} /* namespace model  */
} /* namespace ml */
