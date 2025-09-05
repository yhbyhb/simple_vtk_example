// main.cpp : DICOM volume rendering with presets (soft/bone/lung/bone-only/cinematic)

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

// vtk-dicom
#include <vtkDICOMDirectory.h>
#include <vtkDICOMMetaData.h>
#include <vtkDICOMReader.h>
#include <vtkDICOMTag.h>

// VTK
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

// --------------------------- Transfer Functions -----------------------------

// CT window/level TF (always maps HU -> scalar via slope/intercept)
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
  const double low    = center - width * 0.5;
  const double high   = center + width * 0.5;
  const double mid1   = low + width * 0.25;
  const double mid2   = low + width * 0.75;

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

// Bone-only TF: suppress soft tissue; emphasize trabecular→cortical bone
static void BuildBoneOnlyTransferFunctions(
    vtkColorTransferFunction* ctf,
    vtkPiecewiseFunction* otf,
    double slope,
    double intercept)
{
  const double s = (slope == 0.0 ? 1.0 : slope);
  auto hu2s = [&](double hu)->double { return (hu - intercept) / s; };

  // Soft tissue ~ -100..100 HU; start to reveal ~200+ HU; cortical > ~700 HU
  const double hu0   = 180.0;   // fully transparent below this
  const double hu1   = 250.0;   // begin ramp
  const double hu2   = 700.0;   // cortical onset
  const double hu3   = 1500.0;  // dense bone
  const double huMax = 3000.0;  // clamp top

  // Opacity: zero for soft tissue; ramp for bone
  otf->RemoveAllPoints();
  otf->AddPoint(hu2s(hu0),   0.00);
  otf->AddPoint(hu2s(hu1),   0.02);
  otf->AddPoint(hu2s(hu2),   0.50);
  otf->AddPoint(hu2s(hu3),   0.92);
  otf->AddPoint(hu2s(huMax), 0.98);

  // Color: light bone tones (slightly warm)
  ctf->RemoveAllPoints();
  ctf->AddRGBPoint(hu2s(hu1),   0.85, 0.82, 0.78);
  ctf->AddRGBPoint(hu2s(hu2),   0.92, 0.90, 0.88);
  ctf->AddRGBPoint(hu2s(hu3),   0.98, 0.97, 0.96);
  ctf->AddRGBPoint(hu2s(huMax), 1.00, 1.00, 1.00);
}

// “Cinematic” skull TF: warm translucent tissue, opaque white bone/teeth
static void BuildCinematicSkullTFs(
    vtkColorTransferFunction* ctf,
    vtkPiecewiseFunction* otf,
    double slope,
    double intercept)
{
  const double s = (slope == 0.0 ? 1.0 : slope);
  auto hu2s = [&](double hu)->double { return (hu - intercept) / s; };

  // Key HU landmarks (approximate)
  const double air       = -1000.0;
  const double fat       =  -100.0;
  const double water     =     0.0;
  const double softHi    =   150.0;   // upper soft tissue
  const double trabBone  =   300.0;   // trabecular bone
  const double cortical  =   700.0;   // cortical bone
  const double teeth     =  1500.0;   // enamel/metal
  const double huMax     =  3000.0;

  // Color: amber tissue -> pale bone -> white enamel/metal
  ctf->RemoveAllPoints();
  ctf->AddRGBPoint(hu2s(fat),      0.85, 0.48, 0.20);
  ctf->AddRGBPoint(hu2s(water),    0.92, 0.65, 0.35);
  ctf->AddRGBPoint(hu2s(softHi),   0.95, 0.75, 0.45);
  ctf->AddRGBPoint(hu2s(trabBone), 0.95, 0.90, 0.85);
  ctf->AddRGBPoint(hu2s(cortical), 0.98, 0.97, 0.96);
  ctf->AddRGBPoint(hu2s(teeth),    1.00, 1.00, 1.00);
  ctf->AddRGBPoint(hu2s(huMax),    1.00, 1.00, 1.00);

  // Opacity: tissue translucent, bone ramps to opaque
  otf->RemoveAllPoints();
  otf->AddPoint(hu2s(air),        0.00);
  otf->AddPoint(hu2s(fat),        0.00);
  otf->AddPoint(hu2s(water),      0.05);
  otf->AddPoint(hu2s(softHi),     0.12);
  otf->AddPoint(hu2s(trabBone),   0.35);
  otf->AddPoint(hu2s(cortical),   0.80);
  otf->AddPoint(hu2s(teeth),      0.95);
  otf->AddPoint(hu2s(huMax),      0.98);
}

// ------------------------------- main() -------------------------------------

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <dicom_directory> [--preset soft|bone|lung|bone-only|cinematic]\n";
    return EXIT_FAILURE;
  }

  std::string dicomPath = argv[1];
  std::string preset = "soft"; // default
  bool boneOnly = false;
  bool cinematic = false;

  // Parse args; accept both "--preset bone" and "--preset=bone"
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--bone-only") boneOnly = true;
    else if (a == "--preset" && i + 1 < argc) preset = argv[++i];
    else if (a.rfind("--preset=", 0) == 0)   preset = a.substr(9);
  }

  std::transform(preset.begin(), preset.end(), preset.begin(), ::tolower);
  if (preset == "bone-only") boneOnly = true;
  if (preset == "cinematic") cinematic = true;

  if (!boneOnly && !cinematic &&
      preset != "soft" && preset != "bone" && preset != "lung")
  {
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
    std::cerr << "No DICOM images found under: " << dicomPath << "\n";
    return EXIT_FAILURE;
  }

  vtkNew<vtkDICOMReader> reader;
  // Load the first series found (you can add a --series <idx> later if needed)
  reader->SetFileNames(dicomdir->GetFileNamesForSeries(0));

  try { reader->Update(); }
  catch (...) {
    std::cerr << "Failed to read DICOM series from: " << dicomPath << "\n";
    return EXIT_FAILURE;
  }

  vtkImageData* image = reader->GetOutput();
  if (!image) {
    std::cerr << "No image output.\n";
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
  SPDLOG_INFO("Bone-only: {}", boneOnly ? "true" : "false");
  SPDLOG_INFO("Cinematic: {}", cinematic ? "true" : "false");
  SPDLOG_INFO("Loaded DICOM volume from: {}", dicomPath);
  SPDLOG_INFO("Series found: {}", nSeries);
  SPDLOG_INFO("Extent: [{} , {}] x [{} , {}] x [{} , {}]",
              extent[0], extent[1], extent[2], extent[3], extent[4], extent[5]);
  SPDLOG_INFO("Spacing: ({} , {} , {})", spacing[0], spacing[1], spacing[2]);
  SPDLOG_INFO("Origin:  ({} , {} , {})", origin[0], origin[1], origin[2]);
  SPDLOG_INFO("Scalar range: [{} , {}]", range[0], range[1]);
  SPDLOG_INFO("RescaleSlope={} , RescaleIntercept={}", slope, intercept);

  // ----- Build transfer functions -----
  vtkNew<vtkColorTransferFunction> ctf;
  vtkNew<vtkPiecewiseFunction> otf;

  if (boneOnly) {
    BuildBoneOnlyTransferFunctions(ctf, otf, slope, intercept);
    SPDLOG_INFO("Bone-only mode active: soft tissue suppressed.");
  } else if (cinematic) {
    BuildCinematicSkullTFs(ctf, otf, slope, intercept);
    SPDLOG_INFO("Cinematic preset: warm tissue, white bone/teeth.");
  } else {
    WindowLevel wlww;
    if (preset == "soft") wlww = { 40.0, 400.0 };
    else if (preset == "bone") wlww = { 300.0, 1500.0 };
    else /* lung */ wlww = { -600.0, 1500.0 };

    // Log mapped scalar window
    const double s = (slope == 0.0 ? 1.0 : slope);
    const double lowHU  = wlww.wl - wlww.ww * 0.5;
    const double highHU = wlww.wl + wlww.ww * 0.5;
    const double lowS   = (lowHU  - intercept) / s;
    const double highS  = (highHU - intercept) / s;
    SPDLOG_INFO("Applied WL/WW (HU): WL={} WW={}  -> scalar window [{}, {}]",
                wlww.wl, wlww.ww, lowS, highS);

    BuildCTTransferFunctions(ctf, otf, wlww, slope, intercept);
  }

  // ----- Volume property / lighting -----
  vtkNew<vtkVolumeProperty> vprop;
  vprop->SetColor(ctf);
  vprop->SetScalarOpacity(otf);
  vprop->SetInterpolationTypeToLinear();
  vprop->ShadeOn();
  vprop->SetAmbient(0.2);
  vprop->SetDiffuse(0.9);
  vprop->SetSpecular(0.1);
  vprop->SetSpecularPower(20.0);

  // Gradient opacity to suppress flat/noisy regions
  vtkNew<vtkPiecewiseFunction> gtf;
  gtf->AddPoint(0.0,   0.0);
  gtf->AddPoint(50.0,  0.0);
  gtf->AddPoint(120.0, 0.35);
  gtf->AddPoint(400.0, 1.0);
  vprop->SetGradientOpacity(gtf);

  // Make opacity scale roughly invariant to voxel size
  const double step = std::sqrt(spacing[0]*spacing[0] +
                                spacing[1]*spacing[1] +
                                spacing[2]*spacing[2]);
  vprop->SetScalarOpacityUnitDistance(std::max(0.5, 0.5 * step));

  // ----- Mapper / volume -----
  vtkNew<vtkSmartVolumeMapper> mapper;
  mapper->SetInputData(image);
  mapper->SetBlendModeToComposite();
  mapper->SetAutoAdjustSampleDistances(true);

  vtkNew<vtkVolume> volume;
  volume->SetMapper(mapper);
  volume->SetProperty(vprop);

  // ----- Renderer / window / interactor -----
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
