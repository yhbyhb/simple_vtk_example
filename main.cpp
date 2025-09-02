#include <vtkNew.h>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>

// vtk-dicom
#include <vtkDICOMFileSorter.h>
#include <vtkDICOMReader.h>
#include <vtkDICOMMetaData.h>
#include <vtkDICOMTag.h>

// Volume rendering
#include <vtkStringArray.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkVolume.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>

// logging
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

// macro for performing tests
#define TestAssert(t) \
if (!(t)) \
{ \
  cout << exename << ": Assertion Failed: " << #t << "\n"; \
  cout << __FILE__ << ":" << __LINE__ << "\n"; \
  cout.flush(); \
  rval |= 1; \
}

class ReaderProgress : public vtkCommand
{
public:
  vtkTypeMacro(ReaderProgress, vtkCommand);

  static ReaderProgress *New() { return new ReaderProgress; }

  void Execute(vtkObject *object, unsigned long event, void *data)
    VTK_DICOM_OVERRIDE;
};

void ReaderProgress::Execute(
  vtkObject *object, unsigned long event, void *data)
{
  if (event == vtkCommand::ProgressEvent)
  {
    if (data)
    {
      double progress = *static_cast<double *>(data);
      const char *text = "";
      vtkAlgorithm *algorithm = vtkAlgorithm::SafeDownCast(object);
      if (algorithm)
      {
        text = algorithm->GetProgressText();
      }
      if (text)
      {
        std::cout << text << ": ";
      }
      std::cout << static_cast<int>(100.0*progress + 0.5) << std::endl;
    }
  }
}

struct WindowLevel { double wl; double ww; };

static void BuildCTTransferFunctions(
  vtkColorTransferFunction* ctf,
  vtkPiecewiseFunction* otf,
  const WindowLevel& wlww,
  bool mapHUToScalar,
  double slope,
  double intercept)
{
  auto hu2s = [&](double hu)->double {
    if (!mapHUToScalar) return hu; // assume image scalars are already Hounsfield Units
    double s = (slope == 0.0 ? 1.0 : slope);
    return (hu - intercept) / s; // inverse of (scalar*slope + intercept)
  };

  const double center = wlww.wl;
  const double width = std::max(1.0, wlww.ww);
  const double low = center - width/2.0;
  const double high = center + width/2.0;
  const double mid1 = low + width*0.25;
  const double mid2 = low + width*0.75;

  // Simple grayscale ramp over the window
  ctf->RemoveAllPoints();
  ctf->AddRGBPoint(hu2s(low), 0.0, 0.0, 0.0);
  ctf->AddRGBPoint(hu2s(mid1), 0.5, 0.5, 0.5);
  ctf->AddRGBPoint(hu2s(mid2), 0.8, 0.8, 0.8);
  ctf->AddRGBPoint(hu2s(high), 1.0, 1.0, 1.0);

  // Opacity ramp: nearly transparent below window, more opaque above
  otf->RemoveAllPoints();
  otf->AddPoint(hu2s(low - 200), 0.00);
  otf->AddPoint(hu2s(low), 0.02);
  otf->AddPoint(hu2s(mid1), 0.10);
  otf->AddPoint(hu2s(mid2), 0.35);
  otf->AddPoint(hu2s(high), 0.80);
  otf->AddPoint(hu2s(high + 500), 0.95);
}

int main(int argc, char *argv[])
{
  int rval = 0;
  const char *exename = argv[0];

  std::string preset = "soft"; // default CT window
  bool mapHUToScalar = false; // if true, map HU to raw scalar using slope/intercept

  // remove path portion of exename
  const char *cp = exename + strlen(exename);
  while (cp != exename && cp[-1] != '\\' && cp[-1] != '/') { --cp; }
  exename = cp;

  vtkSmartPointer<vtkDICOMFileSorter> sorter =
    vtkSmartPointer<vtkDICOMFileSorter>::New();

  vtkSmartPointer<vtkStringArray> files =
    vtkSmartPointer<vtkStringArray>::New();

  auto dicomDir = argv[1];

  for (const auto &entry : std::filesystem::directory_iterator(dicomDir))
  {
    if (entry.is_regular_file())
    {
      files->InsertNextValue(entry.path().string());
    }
  }

  sorter->SetInputFileNames(files);
  sorter->Update();

  int m = sorter->GetNumberOfStudies();

  auto reader = vtkSmartPointer<vtkDICOMReader>::New();
  for (int j = 0; j < m; j++)
  {
    cout << "Study" << j << ":\n";
    int k = sorter->GetFirstSeriesForStudy(j);
    int kl = sorter->GetLastSeriesForStudy(j);
    for (; k <= kl; k++)
    {
      cout << "  Series " << k << ":\n";
      vtkStringArray *a = sorter->GetFileNamesForSeries(k);
      auto progressCommand = vtkSmartPointer<ReaderProgress>::New();
      
      reader->AddObserver(vtkCommand::ProgressEvent, progressCommand);
      reader->SetFileNames(a);
      reader->Update();
    }
  }

  try {
    reader->Update();
  } catch (...) {
    std::cerr << "Failed to read DICOM series from: " << dicomDir << std::endl;
    return EXIT_FAILURE;
  }

  auto image = reader->GetOutput();
  if (!image) {
    std::cerr << "No image output." << std::endl;
    return EXIT_FAILURE;
  }

  int extent[6];
  double spacing[3];
  double origin[3];
  image->GetExtent(extent);
  image->GetSpacing(spacing);
  image->GetOrigin(origin);

  double range[2];
  image->GetScalarRange(range);

  // Read rescale slope/intercept from metadata (0028,1053) and (0028,1052)
  double slope = 1.0, intercept = 0.0;
  if (vtkDICOMMetaData* meta = reader->GetMetaData()) {
    vtkDICOMTag RescaleSlope(0x0028, 0x1053);
    vtkDICOMTag RescaleIntercept(0x0028, 0x1052);
    
    if (meta->Has(RescaleSlope)) slope = meta->Get(RescaleSlope).AsDouble();
    if (meta->Has(RescaleIntercept)) intercept = meta->Get(RescaleIntercept).AsDouble();
  }

  SPDLOG_INFO("Loaded DICOM volume from: {}", dicomDir);
  SPDLOG_INFO("Extent: [{} , {}] x [{} , {}] x [{} , {}]", extent[0], extent[1], extent[2], extent[3], extent[4], extent[5]);
  SPDLOG_INFO("Spacing: ({} , {} , {})", spacing[0], spacing[1], spacing[2]);
  SPDLOG_INFO("Origin: ({} , {} , {})", origin[0], origin[1], origin[2]);
  SPDLOG_INFO("Scalar range: [{} , {}]", range[0], range[1]);
  SPDLOG_INFO("RescaleSlope={} , RescaleIntercept={} , mapHUToScalar={}", slope, intercept, mapHUToScalar ? "true" : "false");

  // ----- Build CT preset transfer functions -----
  WindowLevel wlww;
  if (preset == "soft") { wlww = { 40.0, 400.0 }; }
  else if (preset == "bone") { wlww = { 300.0, 1500.0 }; }
  else /* lung */ { wlww = { -600.0, 1500.0 }; }

  vtkNew<vtkColorTransferFunction> ctf;
  vtkNew<vtkPiecewiseFunction> otf;
  BuildCTTransferFunctions(ctf, otf, wlww, mapHUToScalar, slope, intercept);

  vtkNew<vtkVolumeProperty> vprop;
  vprop->SetColor(ctf);
  vprop->SetScalarOpacity(otf);
  vprop->SetInterpolationTypeToLinear();
  vprop->ShadeOn();

  vtkNew<vtkSmartVolumeMapper> mapper;
  mapper->SetInputData(image);
  mapper->SetBlendModeToComposite();

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