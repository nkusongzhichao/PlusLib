/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/

// Local includes
#include "PlusConfigure.h"
#include "vtkPlusSteamVRTracker.h"
#include "vtkPlusDataSource.h"

// VTK includes
#include <vtkMatrix4x4.h>
#include <vtkXMLDataElement.h>
#include <vtkMath.h>
#include <vtkSmartPointer.h>

// STL includes
#include <sstream>

//----------------------------------------------------------------------------

vtkStandardNewMacro(vtkPlusSteamVRTracker);

//----------------------------------------------------------------------------
void vtkPlusSteamVRTracker::PrintSelf(ostream& os, vtkIndent indent)
{
  Superclass::PrintSelf(os, indent);
}

//----------------------------------------------------------------------------
vtkPlusSteamVRTracker::vtkPlusSteamVRTracker()
  : vtkPlusDevice()
  , HMDSource(nullptr)
  , GenericTrackerSource(nullptr)
  , ControllerSource(nullptr)
  , VRContext(nullptr)
  , VRChaperone(nullptr)
  , VROverlay(nullptr)
  , VROverlayHandle(0)
  , HMDRegistered(false)
  , RegisteredControllerCount(0)
  , GenericTrackerRegistered(false)
  , ControllerRegistered(false)
  , SteamVRConnectionTimeout(10.0)
{
  this->FrameNumber = 0;
  this->StartThreadForInternalUpdates = true;
}

//----------------------------------------------------------------------------
vtkPlusSteamVRTracker::~vtkPlusSteamVRTracker()
{
  if (VRContext != nullptr)
  {
    delete VRContext;
    VRContext = nullptr;
  }
  if (VRChaperone != nullptr)
  {
    delete VRChaperone;
    VRChaperone = nullptr;
  }
  if (VROverlay != nullptr)
  {
    delete VROverlay;
    VROverlay = nullptr;
  }

  if (HMDSource != nullptr)
  {
    HMDSource->Delete();
    HMDSource = nullptr;
  }
  if (ControllerSource != nullptr)
  {
    ControllerSource->Delete();
    ControllerSource = nullptr;
  }
  if (GenericTrackerSource != nullptr)
  {
    GenericTrackerSource->Delete();
    GenericTrackerSource = nullptr;
  }
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusSteamVRTracker::Probe()
{
  LOG_TRACE("Searching for Tracking System:")

  if (vr::VR_IsHmdPresent())
  {
    LOG_TRACE("HMD found!");
    return PLUS_SUCCESS;
  }
  else
  {
    LOG_TRACE("No HMD was found in the system.");
    return PLUS_FAIL;
  }
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusSteamVRTracker::InternalConnect()
{
  vr::HmdError err;
  this->VRContext = vr::VR_Init(&err, vr::EVRApplicationType::VRApplication_Scene);

  if (this->VRContext == nullptr)
  {
    LOG_ERROR("Unable to initialize SteamVR system: " vr::VR_GetVRInitErrorAsEnglishDescription(err));
    return PLUS_FAIL;
  }
  else
  {
    LOG_DEBUG("SteamVR runtime successfully initialized");

    int baseStationsCount = 0;
    for (uint32_t td = vr::k_unTrackedDeviceIndex_Hmd; td < vr::k_unMaxTrackedDeviceCount; td++)
    {
      if (this->VRContext->IsTrackedDeviceConnected(td))
      {
        vr::ETrackedDeviceClass tracked_device_class = this->VRContext->GetTrackedDeviceClass(td);

        std::string td_type = GetTrackedDeviceClassString(tracked_device_class);
        this->TrackedDeviceType[td] = td_type;

        if (tracked_device_class == vr::ETrackedDeviceClass::TrackedDeviceClass_TrackingReference)
        {
          baseStationsCount++;
        }
      }
    }

    // Check whether both base stations are found, not mandatory but just in case...
    if (baseStationsCount < 2)
    {
      LOG_ERROR("There was a problem identifying the base stations, please check that they are powered on.");
      return PLUS_FAIL;
    }
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusSteamVRTracker::InternalDisconnect()
{
  LOG_TRACE("Shutting down SteamVR connection.");
  vr::VR_Shutdown();
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusSteamVRTracker::InternalUpdate()
{
  // Obtain tracking device poses
  this->VRContext->GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin::TrackingUniverseStanding, 0, this->TrackedDevicePose, vr::k_unMaxTrackedDeviceCount);

  vtkNew<vtkMatrix4x4> matrix;
  for (vr::TrackedDeviceIndex_t nDevice = 0; nDevice < vr::k_unMaxTrackedDeviceCount; nDevice++)
  {
    if ((this->TrackedDevicePose[nDevice].bDeviceIsConnected) && (this->TrackedDevicePose[nDevice].bPoseIsValid))
    {
      float v[3] = { this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[0][3], this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[1][3], this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[2][3] };
      float r1[3] = { this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[0][0], this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[0][1], this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[0][2] };
      float r2[3] = { this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[1][0], this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[1][1], this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[1][2] };
      float r3[3] = { this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[2][0], this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[2][1], this->TrackedDevicePose[nDevice].mDeviceToAbsoluteTracking.m[2][2] };

      matrix->Identity();
      matrix->Element[0][0] = r1[0];
      matrix->Element[0][1] = r1[1];
      matrix->Element[0][2] = r1[2];
      matrix->Element[1][0] = r2[0];
      matrix->Element[1][1] = r2[1];
      matrix->Element[1][2] = r2[2];
      matrix->Element[2][0] = r3[0];
      matrix->Element[2][1] = r3[1];
      matrix->Element[2][2] = r3[2];
      matrix->Element[0][3] = v[0] * 1000;
      matrix->Element[1][3] = v[1] * 1000;
      matrix->Element[2][3] = v[2] * 1000;

      if (this->VRContext->GetTrackedDeviceClass(nDevice) == TrackedDeviceClass_HMD && this->HMDSource != nullptr)
      {
        this->ToolTimeStampedUpdate(this->HMDSource->GetSourceId(), matrix, ToolStatus(TOOL_OK), this->FrameNumber, NULL, NULL);
      }
      if (this->VRContext->GetTrackedDeviceClass(nDevice) == TrackedDeviceClass_Controller)
      {
        if (this->VRContext->GetControllerRoleForTrackedDeviceIndex(nDevice) == TrackedControllerRole_LeftHand && this->LeftControllerSource != nullptr)
        {
          this->ToolTimeStampedUpdate(this->LeftControllerSource->GetSourceId(), matrix, ToolStatus(TOOL_OK), this->FrameNumber, NULL, NULL);
        }
        else if (this->VRContext->GetControllerRoleForTrackedDeviceIndex(nDevice) == TrackedControllerRole_RightHand && this->RightControllerSource != nullptr)
        {
          this->ToolTimeStampedUpdate(this->RightControllerSource->GetSourceId(), matrix, ToolStatus(TOOL_OK), this->FrameNumber, NULL, NULL);
        }
      }
      else if (this->VRContext->GetTrackedDeviceClass(nDevice) == TrackedDeviceClass_GenericTracker)
      {
        vtkPlusDataSource* aSource(nullptr);
        if (this->GetToolByPortName("GenericTracker", aSource) == PLUS_SUCCESS)
        {
          this->ToolTimeStampedUpdate(aSource->GetSourceId(), matrix, ToolStatus(TOOL_OK), this->FrameNumber, NULL, NULL);
        }
      }
    }
  }
  this->FrameNumber++;
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusSteamVRTracker::NotifyConfigured()
{
  vtkPlusDataSource* aSource(nullptr);
  if (this->GetToolByPortName("HMD", aSource) == PLUS_FAIL)
  {
    LOG_WARNING("Unable to locate tool with port name \"HMD\". Will not record HMD tracking for this session.");
  }
  else
  {
    this->HMDSource = aSource;
  }

  if (this->GetToolByPortName("LeftController", aSource) == PLUS_FAIL)
  {
    LOG_WARNING("Unable to locate tool with port name \"LeftController\". Will not record left controller tracking for this session.");
  }
  else
  {
    this->LeftControllerSource = aSource;
  }

  if (this->GetToolByPortName("RightController", aSource) == PLUS_FAIL)
  {
    LOG_WARNING("Unable to locate tool with port name \"RightController\". Will not record right controller tracking for this session.");
  }
  else
  {
    this->RightControllerSource = aSource;
  }

  return PLUS_SUCCESS;
}
