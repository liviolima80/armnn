﻿//
// Copyright © 2017 Arm Ltd. All rights reserved.
// See LICENSE file in the project root for full license information.
//
#include "InferenceTest.hpp"

#include "InferenceModel.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/assert.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem/operations.hpp>

#include <fstream>
#include <iostream>
#include <iomanip>
#include <array>
#include <chrono>

using namespace std;
using namespace std::chrono;
using namespace armnn::test;

namespace armnn
{
namespace test
{

template <typename TTestCaseDatabase, typename TModel>
ClassifierTestCase<TTestCaseDatabase, TModel>::ClassifierTestCase(
    int& numInferencesRef,
    int& numCorrectInferencesRef,
    const std::vector<unsigned int>& validationPredictions,
    std::vector<unsigned int>* validationPredictionsOut,
    TModel& model,
    unsigned int testCaseId,
    unsigned int label,
    std::vector<typename TModel::DataType> modelInput)
    : InferenceModelTestCase<TModel>(model, testCaseId, std::move(modelInput), model.GetOutputSize())
    , m_Label(label)
    , m_NumInferencesRef(numInferencesRef)
    , m_NumCorrectInferencesRef(numCorrectInferencesRef)
    , m_ValidationPredictions(validationPredictions)
    , m_ValidationPredictionsOut(validationPredictionsOut)
{
}

template <typename TTestCaseDatabase, typename TModel>
TestCaseResult ClassifierTestCase<TTestCaseDatabase, TModel>::ProcessResult(const InferenceTestOptions& params)
{
    auto& output = this->GetOutput();
    const auto testCaseId = this->GetTestCaseId();

    std::map<float,int> resultMap;
    {
        int index = 0;
        for (const auto & o : output)
        {
            resultMap[o] = index++;
        }
    }

    {
        BOOST_LOG_TRIVIAL(info) << "= Prediction values for test #" << testCaseId;
        auto it = resultMap.rbegin();
        for (int i=0; i<5 && it != resultMap.rend(); ++i)
        {
            BOOST_LOG_TRIVIAL(info) << "Top(" << (i+1) << ") prediction is " << it->second <<
              " with confidence: " << 100.0*(it->first) << "%";
            ++it;
        }
    }

    const unsigned int prediction = boost::numeric_cast<unsigned int>(
        std::distance(output.begin(), std::max_element(output.begin(), output.end())));

    // If we're just running the defaultTestCaseIds, each one must be classified correctly
    if (params.m_IterationCount == 0 && prediction != m_Label)
    {
        BOOST_LOG_TRIVIAL(error) << "Prediction for test case " << testCaseId << " (" << prediction << ")" <<
            " is incorrect (should be " << m_Label << ")";
        return TestCaseResult::Failed;
    }

    // If a validation file was provided as input, check that the prediction matches
    if (!m_ValidationPredictions.empty() && prediction != m_ValidationPredictions[testCaseId])
    {
        BOOST_LOG_TRIVIAL(error) << "Prediction for test case " << testCaseId << " (" << prediction << ")" <<
            " doesn't match the prediction in the validation file (" << m_ValidationPredictions[testCaseId] << ")";
        return TestCaseResult::Failed;
    }

    // If a validation file was requested as output, store the predictions
    if (m_ValidationPredictionsOut)
    {
        m_ValidationPredictionsOut->push_back(prediction);
    }

    // Update accuracy stats
    m_NumInferencesRef++;
    if (prediction == m_Label)
    {
        m_NumCorrectInferencesRef++;
    }

    return TestCaseResult::Ok;
}

template <typename TDatabase, typename InferenceModel>
template <typename TConstructDatabaseCallable, typename TConstructModelCallable>
ClassifierTestCaseProvider<TDatabase, InferenceModel>::ClassifierTestCaseProvider(
    TConstructDatabaseCallable constructDatabase, TConstructModelCallable constructModel)
    : m_ConstructModel(constructModel)
    , m_ConstructDatabase(constructDatabase)
    , m_NumInferences(0)
    , m_NumCorrectInferences(0)
{
}

template <typename TDatabase, typename InferenceModel>
void ClassifierTestCaseProvider<TDatabase, InferenceModel>::AddCommandLineOptions(
    boost::program_options::options_description& options)
{
    namespace po = boost::program_options;

    options.add_options()
        ("validation-file-in", po::value<std::string>(&m_ValidationFileIn)->default_value(""),
            "Reads expected predictions from the given file and confirms they match the actual predictions.")
        ("validation-file-out", po::value<std::string>(&m_ValidationFileOut)->default_value(""),
            "Predictions are saved to the given file for later use via --validation-file-in.")
        ("data-dir,d", po::value<std::string>(&m_DataDir)->required(),
            "Path to directory containing test data");

    InferenceModel::AddCommandLineOptions(options, m_ModelCommandLineOptions);
}

template <typename TDatabase, typename InferenceModel>
bool ClassifierTestCaseProvider<TDatabase, InferenceModel>::ProcessCommandLineOptions()
{
    if (!ValidateDirectory(m_DataDir))
    {
        return false;
    }

    ReadPredictions();

    m_Model = m_ConstructModel(m_ModelCommandLineOptions);
    if (!m_Model)
    {
        return false;
    }

    m_Database = std::make_unique<TDatabase>(m_ConstructDatabase(m_DataDir.c_str()));
    if (!m_Database)
    {
        return false;
    }

    return true;
}

template <typename TDatabase, typename InferenceModel>
std::unique_ptr<IInferenceTestCase>
ClassifierTestCaseProvider<TDatabase, InferenceModel>::GetTestCase(unsigned int testCaseId)
{
    std::unique_ptr<typename TDatabase::TTestCaseData> testCaseData = m_Database->GetTestCaseData(testCaseId);
    if (testCaseData == nullptr)
    {
        return nullptr;
    }

    return std::make_unique<ClassifierTestCase<TDatabase, InferenceModel>>(
        m_NumInferences,
        m_NumCorrectInferences,
        m_ValidationPredictions,
        m_ValidationFileOut.empty() ? nullptr : &m_ValidationPredictionsOut,
        *m_Model,
        testCaseId,
        testCaseData->m_Label,
        std::move(testCaseData->m_InputImage));
}

template <typename TDatabase, typename InferenceModel>
bool ClassifierTestCaseProvider<TDatabase, InferenceModel>::OnInferenceTestFinished()
{
    const double accuracy = boost::numeric_cast<double>(m_NumCorrectInferences) /
        boost::numeric_cast<double>(m_NumInferences);
    BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(3) << "Overall accuracy: " << accuracy;

    // If a validation file was requested as output, save the predictions to it
    if (!m_ValidationFileOut.empty())
    {
        std::ofstream validationFileOut(m_ValidationFileOut.c_str(), std::ios_base::trunc | std::ios_base::out);
        if (validationFileOut.good())
        {
            for (const unsigned int prediction : m_ValidationPredictionsOut)
            {
                validationFileOut << prediction << std::endl;
            }
        }
        else
        {
            BOOST_LOG_TRIVIAL(error) << "Failed to open output validation file: " << m_ValidationFileOut;
            return false;
        }
    }

    return true;
}

template <typename TDatabase, typename InferenceModel>
void ClassifierTestCaseProvider<TDatabase, InferenceModel>::ReadPredictions()
{
    // Read expected predictions from the input validation file (if provided)
    if (!m_ValidationFileIn.empty())
    {
        std::ifstream validationFileIn(m_ValidationFileIn.c_str(), std::ios_base::in);
        if (validationFileIn.good())
        {
            while (!validationFileIn.eof())
            {
                unsigned int i;
                validationFileIn >> i;
                m_ValidationPredictions.emplace_back(i);
            }
        }
        else
        {
            throw armnn::Exception(boost::str(boost::format("Failed to open input validation file: %1%")
                % m_ValidationFileIn));
        }
    }
}

template<typename TConstructTestCaseProvider>
int InferenceTestMain(int argc,
    char* argv[],
    const std::vector<unsigned int>& defaultTestCaseIds,
    TConstructTestCaseProvider constructTestCaseProvider)
{
    // Configure logging for both the ARMNN library and this test program
#ifdef NDEBUG
    armnn::LogSeverity level = armnn::LogSeverity::Info;
#else
    armnn::LogSeverity level = armnn::LogSeverity::Debug;
#endif
    armnn::ConfigureLogging(true, true, level);
    armnnUtils::ConfigureLogging(boost::log::core::get().get(), true, true, level);

    try
    {
        std::unique_ptr<IInferenceTestCaseProvider> testCaseProvider = constructTestCaseProvider();
        if (!testCaseProvider)
        {
            return 1;
        }

        InferenceTestOptions inferenceTestOptions;
        if (!ParseCommandLine(argc, argv, *testCaseProvider, inferenceTestOptions))
        {
            return 1;
        }

        const bool success = InferenceTest(inferenceTestOptions, defaultTestCaseIds, *testCaseProvider);
        return success ? 0 : 1;
    }
    catch (armnn::Exception const& e)
    {
        BOOST_LOG_TRIVIAL(fatal) << "Armnn Error: " << e.what();
        return 1;
    }
}

template<typename TDatabase,
    typename TParser,
    typename TConstructDatabaseCallable>
int ClassifierInferenceTestMain(int argc, char* argv[], const char* modelFilename, bool isModelBinary,
    const char* inputBindingName, const char* outputBindingName,
    const std::vector<unsigned int>& defaultTestCaseIds,
    TConstructDatabaseCallable constructDatabase,
    const armnn::TensorShape* inputTensorShape)
{
    return InferenceTestMain(argc, argv, defaultTestCaseIds,
        [=]
        ()
        {
            using InferenceModel = InferenceModel<TParser, float>;
            using TestCaseProvider = ClassifierTestCaseProvider<TDatabase, InferenceModel>;

            return make_unique<TestCaseProvider>(constructDatabase,
                [&]
                (typename InferenceModel::CommandLineOptions modelOptions)
                {
                    if (!ValidateDirectory(modelOptions.m_ModelDir))
                    {
                        return std::unique_ptr<InferenceModel>();
                    }

                    typename InferenceModel::Params modelParams;
                    modelParams.m_ModelPath = modelOptions.m_ModelDir + modelFilename;
                    modelParams.m_InputBinding = inputBindingName;
                    modelParams.m_OutputBinding = outputBindingName;
                    modelParams.m_InputTensorShape = inputTensorShape;
                    modelParams.m_IsModelBinary = isModelBinary;
                    modelParams.m_ComputeDevice = modelOptions.m_ComputeDevice;

                    return std::make_unique<InferenceModel>(modelParams);
            });
        });
}

} // namespace test
} // namespace armnn
