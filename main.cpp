// main.cpp : Simple DICOM volume rendering with VTK
#include <iostream>
#include <string>
#include <algorithm>

// vtk-dicom
#include <vtkDICOMDirectory.h>
#include <vtkDICOMReader.h>
#include <vtkDICOMMetaData.h>
#include <vtkDICOMTag.h>

// vtk
#include <vtkColorTransferFunction.h>
#include <vtkImageData.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkPiecewiseFunction.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

// logging
#include <spdlog/spdlog.h>

struct WindowLevel { double wl; double ww; };

// Always convert HU control points into the image scalar domain using
// DICOM RescaleSlope/RescaleIntercept. This ensures presets work even if
// image scalars are not already HU.
static void BuildCTTransferFunctions(
    vtkColorTransferFunction* ctf,
    vtkPiecewiseFunction* otf,
    const WindowLevel& wlww,
    double slope,
    double intercept)
{
  const double s = (slope == 0.0 ? 1.0 : slope);
  auto hu2s = [&](double hu)->double { return (hu - intercept) / s; };

  const double center = wlww.wl;
  const double width  = std::max(1.0, wlww.ww);
  const double low    = center - width*0.5;
  const double high   = center + width*0.5;
  const double mid1   = low + width*0.25;
  const double mid2   = low + width*0.75;

  // Grayscale ramp
  ctf->RemoveAllPoints();
  ctf->AddRGBPoint(hu2s(low),  0.0, 0.0, 0.0);
  ctf->AddRGBPoint(hu2s(mid1), 0.5, 0.5, 0.5);
  ctf->AddRGBPoint(hu2s(mid2), 0.8, 0.8, 0.8);
  ctf->AddRGBPoint(hu2s(high), 1.0, 1.0, 1.0);

  // Opacity ramp
  otf->RemoveAllPoints();
  otf->AddPoint(hu2s(low   - 200), 0.00);
  otf->AddPoint(hu2s(low),         0.02);
  otf->AddPoint(hu2s(mid1),        0.10);
  otf->AddPoint(hu2s(mid2),        0.35);
  otf->AddPoint(hu2s(high),        0.80);
  otf->AddPoint(hu2s(high + 500),  0.95);
}

// Show only bone: suppress soft tissue (HU < ~200) and ramp opacity for high HU
static void BuildBoneOnlyTransferFunctions(
    vtkColorTransferFunction* ctf,
    vtkPiecewiseFunction* otf,
    double slope,
    double intercept)
{
  // scalar = (HU - intercept) / slope
  const double s = (slope == 0.0 ? 1.0 : slope);
  auto hu2s = [&](double hu)->double { return (hu - intercept) / s; };

  // Soft tissue ~ -100..100 HU; trabecular ~150..300; cortical bone > ~700 HU
  const double hu0   = 150.0;   // below this: fully transparent
  const double hu1   = 250.0;   // start of ramp
  const double hu2   = 700.0;   // cortical starts
  const double hu3   = 1500.0;  // dense bone
  const double huMax = 3000.0;  // clamp top

  // Opacity: zero for soft tissue; ramp up for bone
  otf->RemoveAllPoints();
  otf->AddPoint(hu2s(hu0),   0.00);
  otf->AddPoint(hu2s(hu1),   0.02);
  otf->AddPoint(hu2s(hu2),   0.35);
  otf->AddPoint(hu2s(hu3),   0.85);
  otf->AddPoint(hu2s(huMax), 0.95);

  // Color: light bone tones (slightly warm)
  ctf->RemoveAllPoints();
  ctf->AddRGBPoint(hu2s(hu1),   0.85, 0.82, 0.78);
  ctf->AddRGBPoint(hu2s(hu2),   0.92, 0.90, 0.88);
  ctf->AddRGBPoint(hu2s(hu3),   0.98, 0.97, 0.96);
  ctf->AddRGBPoint(hu2s(huMax), 1.00, 1.00, 1.00);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <dicom_directory> [--preset soft|bone|lung]" << std::endl;
    return EXIT_FAILURE;
  }

  std::string dicomPath = argv[1];
  std::string preset = "soft"; // default CT window

  // Parse args; accept both --preset bone and --preset=bone
  bool boneOnly = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--bone-only") boneOnly = true;
    if (a.rfind("--preset=", 0) == 0) { preset = a.substr(9); }
    else if (a == "--preset" && i+1 < argc) { preset = argv[++i]; }
  }
  if (preset == "bone-only") boneOnly = true;

  std::transform(preset.begin(), preset.end(), preset.begin(), ::tolower);
  if (preset != "soft" && preset != "bone" && preset != "lung" && !boneOnly) {
    std::cerr << "Unknown preset: " << preset << ". Using 'soft'.\n";
    preset = "soft";
  }

  // Scan the directory (recursively) and hand filenames to vtkDICOMReader
  vtkNew<vtkDICOMDirectory> dicomdir;
  dicomdir->SetDirectoryName(dicomPath.c_str());
  dicomdir->RequirePixelDataOn(); // ignore DICOM without pixel data
  dicomdir->Update();

  int nSeries = dicomdir->GetNumberOfSeries();
  if (nSeries < 1) {
    std::cerr << "No DICOM images found under: " << dicomPath << std::endl;
    return EXIT_FAILURE;
  }

  vtkNew<vtkDICOMReader> reader;
  // Load the first series found (you can select via UID if needed)
  reader->SetFileNames(dicomdir->GetFileNamesForSeries(0));

  try { reader->Update(); }
  catch (...) {
    std::cerr << "Failed to read DICOM series from: " << dicomPath << std::endl;
    return EXIT_FAILURE;
  }

  vtkImageData* image = reader->GetOutput();
  if (!image) {
    std::cerr << "No image output." << std::endl;
    return EXIT_FAILURE;
  }

  int extent[6]; double spacing[3]; double origin[3];
  image->GetExtent(extent);
  image->GetSpacing(spacing);
  image->GetOrigin(origin);

  double range[2];
  image->GetScalarRange(range);

  // Rescale slope/intercept (0028,1053) & (0028,1052)
  double slope = 1.0, intercept = 0.0;
  if (vtkDICOMMetaData* meta = reader->GetMetaData()) {
    vtkDICOMTag RescaleSlope(0x0028, 0x1053);
    vtkDICOMTag RescaleIntercept(0x0028, 0x1052);
    if (meta->Has(RescaleSlope))     slope     = meta->Get(RescaleSlope).AsDouble();
    if (meta->Has(RescaleIntercept)) intercept = meta->Get(RescaleIntercept).AsDouble();
  }

  SPDLOG_INFO("Preset: {}", preset);
  SPDLOG_INFO("Loaded DICOM volume from directory: {}", dicomPath);
  SPDLOG_INFO("Series count discovered: {}", nSeries);
  SPDLOG_INFO("Extent: [{} , {}] x [{} , {}] x [{} , {}]",
              extent[0], extent[1], extent[2], extent[3], extent[4], extent[5]);
  SPDLOG_INFO("Spacing: ({} , {} , {})", spacing[0], spacing[1], spacing[2]);
  SPDLOG_INFO("Origin:  ({} , {} , {})", origin[0], origin[1], origin[2]);
  SPDLOG_INFO("Scalar range: [{} , {}]", range[0], range[1]);
  SPDLOG_INFO("RescaleSlope={} , RescaleIntercept={}", slope, intercept);

  // ----- Build CT preset transfer functions -----
  WindowLevel wlww;
  if (preset == "soft") { wlww = { 40.0, 400.0 }; }
  else if (preset == "bone") { wlww = { 300.0, 1500.0 }; }
  else /* lung */ { wlww = { -600.0, 1500.0 }; }

  // Log scalar-domain window bounds after HU->scalar mapping
  const double s = (slope == 0.0 ? 1.0 : slope);
  const double lowHU  = wlww.wl - wlww.ww*0.5;
  const double highHU = wlww.wl + wlww.ww*0.5;
  const double lowS   = (lowHU  - intercept) / s;
  const double highS  = (highHU - intercept) / s;
  SPDLOG_INFO("Applied window/level (HU): WL={} WW={}  -> scalar window [{}, {}]",
              wlww.wl, wlww.ww, lowS, highS);

  vtkNew<vtkColorTransferFunction> ctf;
  vtkNew<vtkPiecewiseFunction> otf;

  if (boneOnly) {
    BuildBoneOnlyTransferFunctions(ctf, otf, slope, intercept);
    SPDLOG_INFO("Bone-only mode: soft tissue suppressed.");
  } else {
    WindowLevel wlww;
    if (preset == "soft") wlww = { 40.0, 400.0 };
    else if (preset == "bone") wlww = { 300.0, 1500.0 };
    else /* lung */ wlww = { -600.0, 1500.0 };
    BuildCTTransferFunctions(ctf, otf, wlww, slope, intercept);
  }

  vtkNew<vtkVolumeProperty> vprop;
  vprop->SetColor(ctf);
  vprop->SetScalarOpacity(otf);
  vprop->SetInterpolationTypeToLinear();
  vprop->ShadeOn();
  vprop->SetAmbient(0.2);
  vprop->SetDiffuse(0.9);
  vprop->SetSpecular(0.1);

  // gradient-based opacity to suppress flat/noisy regions
  vtkNew<vtkPiecewiseFunction> gtf;
  gtf->AddPoint(0.0,   0.0);
  gtf->AddPoint(50.0,  0.0);
  gtf->AddPoint(100.0, 0.3);
  gtf->AddPoint(400.0, 1.0);
  vprop->SetGradientOpacity(gtf);

  vtkNew<vtkSmartVolumeMapper> mapper;
  mapper->SetInputData(image);
  mapper->SetBlendModeToComposite();
  mapper->SetAutoAdjustSampleDistances(true);

  vtkNew<vtkVolume> volume;
  volume->SetMapper(mapper);
  volume->SetProperty(vprop);

  // ----- Renderer setup -----
  vtkNew<vtkRenderer> renderer;
  renderer->SetBackground(0.1, 0.1, 0.12);
  renderer->AddVolume(volume);
  renderer->ResetCamera();

  vtkNew<vtkRenderWindow> renWin;
  renWin->AddRenderer(renderer);
  renWin->SetSize(900, 700);

  vtkNew<vtkRenderWindowInteractor> iren;
  vtkNew<vtkInteractorStyleTrackballCamera> style;
  iren->SetInteractorStyle(style);
  iren->SetRenderWindow(renWin);

  renWin->Render();
  iren->Initialize();
  iren->Start();

  return EXIT_SUCCESS;
}
