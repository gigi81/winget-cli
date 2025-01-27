// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ConfigurationSetApplyProcessor.h"
#include "ConfigurationSetChangeData.h"
#include "ExceptionResultHelpers.h"

#include <AppInstallerErrors.h>
#include <AppInstallerLogging.h>
#include <AppInstallerStrings.h>

namespace winrt::Microsoft::Management::Configuration::implementation
{
    namespace
    {
        std::string GetNormalizedIdentifier(hstring identifier)
        {
            using namespace AppInstaller::Utility;
            return FoldCase(NormalizedString{ identifier });
        }
    }

    ConfigurationSetApplyProcessor::ConfigurationSetApplyProcessor(
        const Configuration::ConfigurationSet& configurationSet,
        const TelemetryTraceLogger& telemetry,
        IConfigurationSetProcessor&& setProcessor,
        AppInstaller::WinRT::AsyncProgress<ApplyConfigurationSetResult, ConfigurationSetChangeData>&& progress) :
            m_configurationSet(configurationSet),
            m_setProcessor(std::move(setProcessor)),
            m_telemetry(telemetry),
            m_result(make_self<wil::details::module_count_wrapper<implementation::ApplyConfigurationSetResult>>()),
            m_progress(std::move(progress))
    {
        // Create a copy of the set of configuration units
        auto unitsView = configurationSet.ConfigurationUnits();
        std::vector<ConfigurationUnit> unitsToProcess{ unitsView.Size() };
        unitsView.GetMany(0, unitsToProcess);

        // Create the unit info vector from these units
        for (const auto& unit : unitsToProcess)
        {
            m_unitInfo.emplace_back(unit);
            m_result->UnitResultsVector().Append(*m_unitInfo.back().Result);
        }

        m_progress.Result(*m_result);
    }

    void ConfigurationSetApplyProcessor::Process()
    {
        try
        {
            if (PreProcess())
            {
                // TODO: Send pending when blocked by another configuration run
                //SendProgress(ConfigurationSetState::Pending);

                SendProgress(ConfigurationSetState::InProgress);

                ProcessInternal(HasProcessedSuccessfully, &ConfigurationSetApplyProcessor::ProcessUnit, true);
            }

            SendProgress(ConfigurationSetState::Completed);

            m_telemetry.LogConfigProcessingSummaryForApply(*winrt::get_self<implementation::ConfigurationSet>(m_configurationSet), *m_result);
        }
        catch (...)
        {
            const auto& configurationSet = *winrt::get_self<implementation::ConfigurationSet>(m_configurationSet);
            m_telemetry.LogConfigProcessingSummary(
                configurationSet.InstanceIdentifier(),
                configurationSet.IsFromHistory(),
                ConfigurationUnitIntent::Apply,
                LOG_CAUGHT_EXCEPTION(),
                ConfigurationUnitResultSource::Internal,
                GetProcessingSummaryFor(ConfigurationUnitIntent::Assert),
                GetProcessingSummaryFor(ConfigurationUnitIntent::Inform),
                GetProcessingSummaryFor(ConfigurationUnitIntent::Apply));
            throw;
        }
    }

    Configuration::ApplyConfigurationSetResult ConfigurationSetApplyProcessor::Result() const
    {
        return *m_result;
    }

    ConfigurationSetApplyProcessor::UnitInfo::UnitInfo(const Configuration::ConfigurationUnit& unit) :
        Unit(unit), Result(make_self<wil::details::module_count_wrapper<implementation::ApplyConfigurationUnitResult>>()),
        ResultInformation(make_self<wil::details::module_count_wrapper<implementation::ConfigurationUnitResultInformation>>())
    {
        Result->Unit(unit);
        Result->ResultInformation(*ResultInformation);
    }

    bool ConfigurationSetApplyProcessor::PreProcess()
    {
        bool result = true;

        for (size_t i = 0; i < m_unitInfo.size(); ++i)
        {
            if (!AddUnitToMap(m_unitInfo[i], i))
            {
                result = false;
            }
        }

        if (!result)
        {
            // This is the only error that adding to the map can produce
            m_result->ResultCode(WINGET_CONFIG_ERROR_DUPLICATE_IDENTIFIER);
            return false;
        }

        for (UnitInfo& unitInfo : m_unitInfo)
        {
            for (hstring dependencyHstring : unitInfo.Unit.Dependencies())
            {
                // Throw out empty dependency strings
                if (dependencyHstring.empty())
                {
                    continue;
                }

                std::string dependency = GetNormalizedIdentifier(dependencyHstring);
                auto itr = m_idToUnitInfoIndex.find(dependency);
                if (itr == m_idToUnitInfoIndex.end())
                {
                    AICLI_LOG(Config, Error, << "Found missing dependency: " << dependency);
                    unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_MISSING_DEPENDENCY, ConfigurationUnitResultSource::ConfigurationSet);
                    unitInfo.ResultInformation->Details(dependencyHstring);
                    SendProgress(ConfigurationUnitState::Completed, unitInfo);
                    result = false;
                    // TODO: Consider collecting all missing dependencies, for now just the first
                    break;
                }
                else
                {
                    unitInfo.DependencyIndices.emplace_back(itr->second);
                }
            }
        }

        if (!result)
        {
            // This is the only error that adding to the map can produce
            m_result->ResultCode(WINGET_CONFIG_ERROR_MISSING_DEPENDENCY);
            return false;
        }

        if (!ProcessInternal(HasPreprocessed, &ConfigurationSetApplyProcessor::MarkPreprocessed))
        {
            // The preprocessing simulates processing as if every unit run was successful.
            // If it fails, this means that there are unit definitions whose dependencies cannot be satisfied.
            // The only reason for that is a cycle in the dependency graph somewhere.
            m_result->ResultCode(WINGET_CONFIG_ERROR_SET_DEPENDENCY_CYCLE);
            return false;
        }

        return true;
    }

    bool ConfigurationSetApplyProcessor::AddUnitToMap(UnitInfo& unitInfo, size_t unitInfoIndex)
    {
        hstring originalIdentifier = unitInfo.Unit.Identifier();
        if (originalIdentifier.empty())
        {
            return true;
        }

        std::string identifier = GetNormalizedIdentifier(originalIdentifier);

        auto itr = m_idToUnitInfoIndex.find(identifier);
        if (itr != m_idToUnitInfoIndex.end())
        {
            AICLI_LOG(Config, Error, << "Found duplicate identifier: " << identifier);
            // Found a duplicate identifier, mark both as such
            m_unitInfo[itr->second].ResultInformation->Initialize(WINGET_CONFIG_ERROR_DUPLICATE_IDENTIFIER, ConfigurationUnitResultSource::ConfigurationSet);
            SendProgressIfNotComplete(ConfigurationUnitState::Completed, m_unitInfo[itr->second]);
            unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_DUPLICATE_IDENTIFIER, ConfigurationUnitResultSource::ConfigurationSet);
            SendProgress(ConfigurationUnitState::Completed, unitInfo);
            return false;
        }
        else
        {
            m_idToUnitInfoIndex.emplace(std::move(identifier), unitInfoIndex);
            return true;
        }
    }

    bool ConfigurationSetApplyProcessor::ProcessInternal(CheckDependencyPtr checkDependencyFunction, ProcessUnitPtr processUnitFunction, bool sendProgress)
    {
        // Create the set of units that need to be processed
        std::vector<size_t> unitsToProcess;
        for (size_t i = 0, size = m_unitInfo.size(); i < size; ++i)
        {
            unitsToProcess.emplace_back(i);
        }

        // Always process all ConfigurationUnitIntent::Assert first
        if (!ProcessIntentInternal(
            unitsToProcess,
            checkDependencyFunction,
            processUnitFunction,
            ConfigurationUnitIntent::Assert,
            WINGET_CONFIG_ERROR_ASSERTION_FAILED,
            WINGET_CONFIG_ERROR_ASSERTION_FAILED,
            sendProgress))
        {
            return false;
        }

        // Then all ConfigurationUnitIntent::Inform
        if (!ProcessIntentInternal(
            unitsToProcess,
            checkDependencyFunction,
            processUnitFunction,
            ConfigurationUnitIntent::Inform,
            WINGET_CONFIG_ERROR_DEPENDENCY_UNSATISFIED,
            WINGET_CONFIG_ERROR_DEPENDENCY_UNSATISFIED,
            sendProgress))
        {
            return false;
        }

        // Then all ConfigurationUnitIntent::Apply
        return ProcessIntentInternal(
            unitsToProcess,
            checkDependencyFunction,
            processUnitFunction,
            ConfigurationUnitIntent::Apply,
            E_FAIL, // This should not happen as there are no other intents left
            WINGET_CONFIG_ERROR_SET_APPLY_FAILED,
            sendProgress);
    }

    bool ConfigurationSetApplyProcessor::ProcessIntentInternal(
        std::vector<size_t>& unitsToProcess,
        CheckDependencyPtr checkDependencyFunction,
        ProcessUnitPtr processUnitFunction,
        ConfigurationUnitIntent intent,
        hresult errorForOtherIntents,
        hresult errorForFailures,
        bool sendProgress)
    {
        // Always process the first item in the list that is available to be processed
        bool hasProcessed = true;
        bool hasFailure = false;
        while (hasProcessed)
        {
            hasProcessed = false;
            for (auto itr = unitsToProcess.begin(), end = unitsToProcess.end(); itr != end; ++itr)
            {
                UnitInfo& unitInfo = m_unitInfo[*itr];
                if (HasIntentAndSatisfiedDependencies(unitInfo, intent, checkDependencyFunction))
                {
                    if (!(this->*processUnitFunction)(unitInfo))
                    {
                        hasFailure = true;
                    }
                    unitsToProcess.erase(itr);
                    hasProcessed = true;
                    break;
                }
            }
        }

        // Mark all remaining items with intent as failed due to dependency
        bool hasRemainingDependencies = false;
        for (size_t index : unitsToProcess)
        {
            UnitInfo& unitInfo = m_unitInfo[index];
            if (unitInfo.Unit.Intent() == intent)
            {
                hasRemainingDependencies = true;
                unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_DEPENDENCY_UNSATISFIED, ConfigurationUnitResultSource::Precondition);
                if (sendProgress)
                {
                    SendProgress(ConfigurationUnitState::Skipped, unitInfo);
                }
            }
        }

        // Any failures are fatal, mark all other units as failed due to that
        if (hasFailure || hasRemainingDependencies)
        {
            for (size_t index : unitsToProcess)
            {
                UnitInfo& unitInfo = m_unitInfo[index];
                if (unitInfo.Unit.Intent() != intent)
                {
                    unitInfo.ResultInformation->Initialize(errorForOtherIntents, ConfigurationUnitResultSource::Precondition);
                    if (sendProgress)
                    {
                        SendProgress(ConfigurationUnitState::Skipped, unitInfo);
                    }
                }
            }

            if (hasFailure)
            {
                m_result->ResultCode(errorForFailures);
            }
            else // hasRemainingDependencies
            {
                m_result->ResultCode(WINGET_CONFIG_ERROR_DEPENDENCY_UNSATISFIED);
            }
            return false;
        }

        return true;
    }

    bool ConfigurationSetApplyProcessor::HasIntentAndSatisfiedDependencies(
        const UnitInfo& unitInfo,
        ConfigurationUnitIntent intent,
        CheckDependencyPtr checkDependencyFunction) const
    {
        bool result = false;

        if (unitInfo.Unit.Intent() == intent)
        {
            result = true;
            for (size_t dependencyIndex : unitInfo.DependencyIndices)
            {
                if (!checkDependencyFunction(m_unitInfo[dependencyIndex]))
                {
                    result = false;
                    break;
                }
            }
        }

        return result;
    }

    bool ConfigurationSetApplyProcessor::HasPreprocessed(const UnitInfo& unitInfo)
    {
        return unitInfo.PreProcessed;
    }

    bool ConfigurationSetApplyProcessor::MarkPreprocessed(UnitInfo& unitInfo)
    {
        unitInfo.PreProcessed = true;
        return true;
    }

    bool ConfigurationSetApplyProcessor::HasProcessedSuccessfully(const UnitInfo& unitInfo)
    {
        return unitInfo.Processed && SUCCEEDED(unitInfo.ResultInformation->ResultCode());
    }

    bool ConfigurationSetApplyProcessor::ProcessUnit(UnitInfo& unitInfo)
    {
        m_progress.ThrowIfCancelled();

        IConfigurationUnitProcessor unitProcessor;

        // Once we get this far, consider the unit processed even if we fail to create the actual processor.
        unitInfo.Processed = true;

        if (!unitInfo.Unit.ShouldApply())
        {
            // If the unit is requested to be skipped, we mark it with a failure to prevent any dependency from running.
            // But we return true from this function to indicate a successful "processing".
            unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_MANUALLY_SKIPPED, ConfigurationUnitResultSource::Precondition);
            SendProgress(ConfigurationUnitState::Skipped, unitInfo);
            return true;
        }

        // Send a progress event that we are starting, and prepare one for completion when we exit the function
        SendProgress(ConfigurationUnitState::InProgress, unitInfo);
        auto sendCompletedProgress = wil::scope_exit([this, &unitInfo]() { SendProgress(ConfigurationUnitState::Completed, unitInfo); });

        try
        {
            unitProcessor = m_setProcessor.CreateUnitProcessor(unitInfo.Unit, {});
        }
        catch (...)
        {
            ExtractUnitResultInformation(std::current_exception(), unitInfo.ResultInformation);
            return false;
        }

        // As the process of creating the unit processor could take a while, check for cancellation again
        m_progress.ThrowIfCancelled();

        bool result = false;
        std::string_view action;

        try
        {
            switch (unitInfo.Unit.Intent())
            {
            case ConfigurationUnitIntent::Assert:
            {
                action = TelemetryTraceLogger::TestAction;
                TestSettingsResult settingsResult = unitProcessor.TestSettings();

                if (settingsResult.TestResult() == ConfigurationTestResult::Positive)
                {
                    result = true;
                }
                else if (settingsResult.TestResult() == ConfigurationTestResult::Negative)
                {
                    unitInfo.ResultInformation->Initialize(WINGET_CONFIG_ERROR_ASSERTION_FAILED, ConfigurationUnitResultSource::Precondition);
                }
                else if (settingsResult.TestResult() == ConfigurationTestResult::Failed)
                {
                    unitInfo.ResultInformation->Initialize(settingsResult.ResultInformation());
                }
                else
                {
                    unitInfo.ResultInformation->Initialize(E_UNEXPECTED, ConfigurationUnitResultSource::Internal);
                }
            }
                break;

            case ConfigurationUnitIntent::Inform:
            {
                // Force the processor to retrieve the settings
                action = TelemetryTraceLogger::GetAction;
                GetSettingsResult settingsResult = unitProcessor.GetSettings();
                if (SUCCEEDED(settingsResult.ResultInformation().ResultCode()))
                {
                    result = true;
                }
                else
                {
                    unitInfo.ResultInformation->Initialize(settingsResult.ResultInformation());
                }
            }
                break;

            case ConfigurationUnitIntent::Apply:
            {
                action = TelemetryTraceLogger::TestAction;
                TestSettingsResult testSettingsResult = unitProcessor.TestSettings();

                if (testSettingsResult.TestResult() == ConfigurationTestResult::Positive)
                {
                    unitInfo.Result->PreviouslyInDesiredState(true);
                    result = true;
                }
                else if (testSettingsResult.TestResult() == ConfigurationTestResult::Negative)
                {
                    // Just in case testing took a while, check for cancellation before moving on to applying
                    m_progress.ThrowIfCancelled();

                    action = TelemetryTraceLogger::ApplyAction;
                    ApplySettingsResult applySettingsResult = unitProcessor.ApplySettings();
                    if (SUCCEEDED(applySettingsResult.ResultInformation().ResultCode()))
                    {
                        unitInfo.Result->RebootRequired(applySettingsResult.RebootRequired());
                        result = true;
                    }
                    else
                    {
                        unitInfo.ResultInformation->Initialize(applySettingsResult.ResultInformation());
                    }
                }
                else if (testSettingsResult.TestResult() == ConfigurationTestResult::Failed)
                {
                    unitInfo.ResultInformation->Initialize(testSettingsResult.ResultInformation());
                }
                else
                {
                    unitInfo.ResultInformation->Initialize(E_UNEXPECTED, ConfigurationUnitResultSource::Internal);
                }
            }
                break;

            default:
                unitInfo.ResultInformation->Initialize(E_UNEXPECTED, ConfigurationUnitResultSource::Internal);
                break;
            }
        }
        catch (...)
        {
            ExtractUnitResultInformation(std::current_exception(), unitInfo.ResultInformation);
        }

        m_telemetry.LogConfigUnitRunIfAppropriate(m_configurationSet.InstanceIdentifier(), unitInfo.Unit, ConfigurationUnitIntent::Apply, action, *unitInfo.ResultInformation);
        return result;
    }

    void ConfigurationSetApplyProcessor::SendProgress(ConfigurationSetState state)
    {
        try
        {
            m_progress.Progress(implementation::ConfigurationSetChangeData::Create(state));
        }
        CATCH_LOG();
    }

    void ConfigurationSetApplyProcessor::SendProgress(ConfigurationUnitState state, const UnitInfo& unitInfo)
    {
        unitInfo.Result->State(state);

        try
        {
            m_progress.Progress(implementation::ConfigurationSetChangeData::Create(state, *unitInfo.ResultInformation, unitInfo.Unit));
        }
        CATCH_LOG();
    }

    void ConfigurationSetApplyProcessor::SendProgressIfNotComplete(ConfigurationUnitState state, const UnitInfo& unitInfo)
    {
        if (unitInfo.Result->State() != ConfigurationUnitState::Completed)
        {
            SendProgress(state, unitInfo);
        }
    }

    TelemetryTraceLogger::ProcessingSummaryForIntent ConfigurationSetApplyProcessor::GetProcessingSummaryFor(ConfigurationUnitIntent intent) const
    {
        TelemetryTraceLogger::ProcessingSummaryForIntent result{ intent, 0, 0, 0 };

        for (const auto& unitInfo : m_unitInfo)
        {
            if (unitInfo.Unit.Intent() == intent)
            {
                ++result.Count;

                if (unitInfo.Processed)
                {
                    ++result.Run;

                    if (FAILED(unitInfo.ResultInformation->ResultCode()))
                    {
                        ++result.Failed;
                    }
                }
            }
        }

        return result;
    }
}
