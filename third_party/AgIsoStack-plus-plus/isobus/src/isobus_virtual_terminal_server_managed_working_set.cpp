//================================================================================================
/// @file isobus_virtual_terminal_server_managed_working_set.cpp
///
/// @brief Defines a class that manages a VT server's active working sets.
/// @author Adrian Del Grosso
///
/// @copyright 2023 Adrian Del Grosso
//================================================================================================
#include "isobus/isobus/isobus_virtual_terminal_server_managed_working_set.hpp"

#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/utility/to_string.hpp"

#include <cstring>
#if ISOVT_ESP_POOL_THREADS
#include "esp_pthread.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
namespace isobus
{
	VirtualTerminalServerManagedWorkingSet::VirtualTerminalServerManagedWorkingSet()
	{
		LOG_INFO("[WS]: New VT Server Object Created with no associated control function");
	}

	VirtualTerminalServerManagedWorkingSet::VirtualTerminalServerManagedWorkingSet(std::shared_ptr<ControlFunction> associatedControlFunction) :
	  workingSetControlFunction(associatedControlFunction)
	{
		if (nullptr != associatedControlFunction)
		{
			LOG_INFO("[WS]: New VT Server Object Created for CF " + to_string(static_cast<int>(associatedControlFunction->get_NAME().get_full_name())));
		}
		else
		{
			LOG_INFO("[WS]: New VT Server Object Created with no associated control function");
		}
	}

	void VirtualTerminalServerManagedWorkingSet::start_parsing_thread()
	{
#if ISOVT_ESP_POOL_THREADS
		if (objectPoolProcessingThreadStarted)
		{
			return;
		}
		auto previousConfig = esp_pthread_get_default_config();
		esp_pthread_get_cfg(&previousConfig);
		auto parserConfig = previousConfig;
		// Parsing only accesses RAM; keep scarce internal RAM for CAN, UART and DMA.
		parserConfig.stack_size = 24576;
		parserConfig.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
		parserConfig.thread_name = "vt_pool";
		parserConfig.inherit_cfg = false;
		if (esp_pthread_set_cfg(&parserConfig) != ESP_OK)
		{
			set_object_pool_processing_state(ObjectPoolProcessingThreadState::Fail);
			LOG_ERROR("[WS]: Unable to configure object-pool parser thread.");
			return;
		}
		set_object_pool_processing_state(ObjectPoolProcessingThreadState::Running);
		// std::thread aborts on creation failure when C++ exceptions are disabled.
		const int result = pthread_create(&objectPoolProcessingThread, nullptr, [](void *context) -> void * {
			auto *workingSet = static_cast<VirtualTerminalServerManagedWorkingSet *>(context);
			workingSet->worker_thread_function();
			LOG_INFO("[WS]: Parser stack free: %u bytes", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
			return nullptr;
		}, this);
		esp_pthread_set_cfg(&previousConfig);
		objectPoolProcessingThreadStarted = (result == 0);
		if (result != 0)
		{
			set_object_pool_processing_state(ObjectPoolProcessingThreadState::Fail);
			LOG_ERROR("[WS]: Unable to start object-pool parser (%d), internal free=%u largest=%u", result,
			          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
			          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
		}
#else
		if (nullptr == objectPoolProcessingThread)
		{
			objectPoolProcessingThread.reset(new std::thread([this]() { worker_thread_function(); }));
		}
#endif
	}

	void VirtualTerminalServerManagedWorkingSet::join_parsing_thread()
	{
#if ISOVT_ESP_POOL_THREADS
		if (objectPoolProcessingThreadStarted)
		{
			pthread_join(objectPoolProcessingThread, nullptr);
			objectPoolProcessingThreadStarted = false;
		}
		// Also finish a failed creation so the server sends only one failure reply.
		set_object_pool_processing_state(ObjectPoolProcessingThreadState::Joined);
#else
		if ((nullptr != objectPoolProcessingThread) && (objectPoolProcessingThread->joinable()))
		{
			objectPoolProcessingThread->join();
			objectPoolProcessingThread = nullptr;
			set_object_pool_processing_state(ObjectPoolProcessingThreadState::Joined);
		}
#endif
	}

	bool VirtualTerminalServerManagedWorkingSet::get_any_object_pools() const
	{
		return (!iopFilesRawData.empty());
	}

	VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState VirtualTerminalServerManagedWorkingSet::get_object_pool_processing_state()
	{
		return processingState.load(std::memory_order_acquire);
	}

	std::shared_ptr<ControlFunction> VirtualTerminalServerManagedWorkingSet::get_control_function() const
	{
		return workingSetControlFunction;
	}

	std::uint32_t VirtualTerminalServerManagedWorkingSet::get_working_set_maintenance_message_timestamp_ms() const
	{
		return workingSetMaintenanceMessageTimestamp_ms;
	}

	void VirtualTerminalServerManagedWorkingSet::set_working_set_maintenance_message_timestamp_ms(std::uint32_t value)
	{
		workingSetMaintenanceMessageTimestamp_ms = value;
	}

	void VirtualTerminalServerManagedWorkingSet::save_callback_handle(isobus::EventCallbackHandle callbackHandle)
	{
		callbackHandles.push_back(callbackHandle);
	}

	void VirtualTerminalServerManagedWorkingSet::clear_callback_handles()
	{
		callbackHandles.clear();
	}

	bool VirtualTerminalServerManagedWorkingSet::get_was_object_pool_loaded_from_non_volatile_memory() const
	{
		return wasLoadedFromNonVolatileMemory;
	}

	void VirtualTerminalServerManagedWorkingSet::set_was_object_pool_loaded_from_non_volatile_memory(bool value, CANLibBadge<VirtualTerminalServer>)
	{
		wasLoadedFromNonVolatileMemory = value;
	}

	void VirtualTerminalServerManagedWorkingSet::set_object_focus(std::uint16_t objectID)
	{
		focusedObject = objectID;
	}

	std::uint16_t VirtualTerminalServerManagedWorkingSet::get_object_focus() const
	{
		return focusedObject;
	}

	void VirtualTerminalServerManagedWorkingSet::set_auxiliary_input_maintenance_timestamp_ms(std::uint32_t value)
	{
		auxiliaryInputMaintenanceMessageTimestamp_ms = value;
	}

	std::uint32_t VirtualTerminalServerManagedWorkingSet::get_auxiliary_input_maintenance_timestamp_ms() const
	{
		return auxiliaryInputMaintenanceMessageTimestamp_ms;
	}

	void VirtualTerminalServerManagedWorkingSet::request_deletion()
	{
		workingSetDeletionRequested = true;
	}

	bool VirtualTerminalServerManagedWorkingSet::is_deletion_requested() const
	{
		return workingSetDeletionRequested;
	}

	void VirtualTerminalServerManagedWorkingSet::set_iop_size(std::uint32_t newIopSize)
	{
		const std::lock_guard<std::mutex> lock(managedWorkingSetMutex);
		iopSize = newIopSize;
	}

	float VirtualTerminalServerManagedWorkingSet::iop_load_percentage() const
	{
		if (processingState != ObjectPoolProcessingThreadState::None || transferredIopSize > iopSize)
		{
			return 100.0f;
		}

		if (0 == iopSize)
		{
			return 0.0f;
		}

		// if IOP transfer is not completed check if there is an ongoing IOP transfer to us
		auto sessions = CANNetworkManager::CANNetwork.get_active_transport_protocol_sessions(0);
		auto currentTransferredIopSize = transferredIopSize;
		for (const auto &session : sessions)
		{
			if (session->get_source()->get_address() == get_control_function()->get_address() &&
			    (static_cast<std::uint32_t>(CANLibParameterGroupNumber::ECUtoVirtualTerminal) == session->get_parameter_group_number()) &&
			    (session->get_data().size() >= 1) &&
			    (session->get_data().get_byte(0) == 0x11)) // ObjectPoolTransferMessage
			{
				currentTransferredIopSize += session->get_total_bytes_transferred();
			}
		}

		if (currentTransferredIopSize > iopSize)
		{
			return 100.0f;
		}
		return (static_cast<float>(currentTransferredIopSize) / static_cast<float>(iopSize)) * 100.0f;
	}

	void VirtualTerminalServerManagedWorkingSet::set_object_pool_processing_state(ObjectPoolProcessingThreadState value)
	{
		processingState.store(value, std::memory_order_release);
	}

	void VirtualTerminalServerManagedWorkingSet::worker_thread_function()
	{
		if (!iopFilesRawData.empty())
		{
			bool lSuccess = true;

			set_object_pool_processing_state(ObjectPoolProcessingThreadState::Running);
			LOG_INFO("[WS]: Beginning parsing of object pool. This pool has " +
			         isobus::to_string(static_cast<int>(iopFilesRawData.size())) +
			         " IOP components.");
			for (std::size_t i = 0; i < iopFilesRawData.size(); i++)
			{
				if (!parse_iop_into_objects(iopFilesRawData[i].data(), static_cast<std::uint32_t>(iopFilesRawData[i].size())))
				{
					lSuccess = false;
					break;
				}
			}

			if (lSuccess)
			{
				LOG_INFO("[WS]: Object pool successfully parsed.");
				set_object_pool_processing_state(ObjectPoolProcessingThreadState::Success);
			}
			else
			{
				LOG_ERROR("[WS]: Object pool failed to be parsed.");
				set_object_pool_processing_state(ObjectPoolProcessingThreadState::Fail);
			}
		}
		else
		{
			LOG_ERROR("[WS]: Object pool failed to be parsed.");
			set_object_pool_processing_state(ObjectPoolProcessingThreadState::Fail);
		}
	}

	bool VirtualTerminalServerManagedWorkingSet::is_object_pool_transfer_in_progress() const
	{
		return iop_load_percentage() != 0.0f;
	}

} // namespace isobus
