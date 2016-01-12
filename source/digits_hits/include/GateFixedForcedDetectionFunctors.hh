#ifndef GATEFIXEDFORCEDDETECTIONACTORFUNCTORS_HH
#define GATEFIXEDFORCEDDETECTIONACTORFUNCTORS_HH

// Geant4
#include <G4VEMDataSet.hh>
#include <G4EmCalculator.hh>
#include <G4VDataSetAlgorithm.hh>
#include <G4LivermoreComptonModel.hh>
#include <G4LogLogInterpolation.hh>
#include <G4CompositeEMDataSet.hh>
#include <G4CrossSectionHandler.hh>
#include "G4Poisson.hh"
#include "GateEnergyResponseFunctor.hh"

// ITK
#include <itkImage.h>
#include <itkImageRegionIterator.h>
#include <itkTimeProbe.h>

// RTK
#include <rtkConfiguration.h>

namespace GateFixedForcedDetectionFunctor
{

//-----------------------------------------------------------------------------
// Handling of the interpolation weight in primary: store the weights and
// the material indices in vectors and return nada. The integral is computed in the
// ProjectedValueAccumulation since one has to repeat the same ray cast for each
// and every energy of the primary.
class InterpolationWeightMultiplication
{
public:
  typedef itk::Vector<double, 3> VectorType;

  InterpolationWeightMultiplication() {};
  ~InterpolationWeightMultiplication() {};
  bool operator!=( const InterpolationWeightMultiplication & ) const {
    return false;
  }
  bool operator==(const InterpolationWeightMultiplication & other) const {
    return !( *this != other );
  }

  inline double operator()( const rtk::ThreadIdType threadId,
                            const double stepLengthInVoxel,
                            const double weight,
                            const float *p,
                            const int i) {
    m_InterpolationWeights[threadId][(int)(p[i])] += stepLengthInVoxel * weight;
    return 0.;
  }

  std::vector<double>* GetInterpolationWeights() { return m_InterpolationWeights; }

private:
  std::vector<double> m_InterpolationWeights[ITK_MAX_THREADS];
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Mother class for accumulation. Purely virtual (FIXME).
template<bool VIsEnergyResolved = false>
class VAccumulation
{
public:
  static const unsigned int Dimension = 3;
  typedef itk::Vector<double, Dimension>        VectorType;
  typedef float                                 InputPixelType;
  typedef itk::Image<InputPixelType, Dimension> InputImageType;
  typedef itk::Image<double, 2>                 MaterialMuImageType;

  VAccumulation():
    m_NumberOfPrimaries(0),
    m_EnergyResolvedBinSize(0.)
  {
    for(int i=0; i<ITK_MAX_THREADS; i++)
      {
      m_IntegralOverDetector[i] = 0.;
      m_SquaredIntegralOverDetector[i] = 0.;
      }
  }

  bool operator!=( const VAccumulation & ) const
  {
    return false;
  }

  bool operator==(const VAccumulation & other) const
  {
    return !( *this != other );
  }
  void SetVolumeSpacing(const VectorType &_arg){ m_VolumeSpacing = _arg; }
  void SetInterpolationWeights(std::vector<double> *_arg){ m_InterpolationWeights = _arg; }
  void SetEnergyWeightList(std::vector<double> *_arg) { m_EnergyWeightList = _arg; }
  void Init(unsigned int nthreads) {
    for(unsigned int i=0; i<nthreads; i++) {
      m_InterpolationWeights[i].resize(m_MaterialMu->GetLargestPossibleRegion().GetSize()[0]);
      std::fill(m_InterpolationWeights[i].begin(), m_InterpolationWeights[i].end(), 0.);
    }
  }

  // Solid angle from the source to pixel vector in voxels
  void SetSolidAngleParameters(const typename InputImageType::Pointer proj,
                               const VectorType &u,
                               const VectorType &v) {
    m_DetectorOrientationTimesPixelSurface = proj->GetSpacing()[0] *
                                             proj->GetSpacing()[1] *
                                             itk::CrossProduct(u,v);
  }
  double GetSolidAngle(const VectorType &sourceToPixelInVox) const {
    VectorType sourceToPixelInMM;
    for(int i=0; i<3; i++)
      sourceToPixelInMM[i] =  sourceToPixelInVox[i]*m_VolumeSpacing[i];
    return std::abs(sourceToPixelInMM * m_DetectorOrientationTimesPixelSurface / pow(sourceToPixelInMM.GetNorm(), 3.));
  }

  MaterialMuImageType *GetMaterialMu() { return m_MaterialMu.GetPointer(); }

  void CreateMaterialMuMap(G4EmCalculator *emCalculator,
                           const double energySpacing,
                           const double energyMax,
                           GateVImageVolume * gate_image_volume) {
    std::vector<double> energyList;
    energyList.push_back(0.);
    while(energyList.back()<energyMax)
      energyList.push_back(energyList.back()+energySpacing);
    CreateMaterialMuMap(emCalculator, energyList, gate_image_volume);
    MaterialMuImageType::SpacingType spacing;
    spacing[0] = 1.;
    spacing[1] = energySpacing;
    m_MaterialMu->SetSpacing(spacing);
    MaterialMuImageType::PointType origin;
    origin.Fill(0.);
    m_MaterialMu->SetOrigin(origin);
  }

  void CreateMaterialMuMap(G4EmCalculator *emCalculator,
                           const std::vector<double> &Elist,
                           GateVImageVolume * gate_image_volume) {
    m_EnergyList = Elist;
    itk::TimeProbe muProbe;
    muProbe.Start();

    // Get image materials + world
    std::vector<G4Material*> m;
    gate_image_volume->BuildLabelToG4MaterialVector(m);
    GateVVolume *v = gate_image_volume;
    while (v->GetLogicalVolumeName() != "world_log")
      v = v->GetParentVolume();
    m.push_back(const_cast<G4Material*>(v->GetMaterial()));

    // Get the list of involved processes (Rayleigh, Compton, PhotoElectric)
    G4ParticleDefinition* particle = G4ParticleTable::GetParticleTable()->FindParticle("gamma");
    G4ProcessVector* plist = particle->GetProcessManager()->GetProcessList();
    std::vector<G4String> processNameVector;
    for (G4int j = 0; j < plist->size(); j++) {
      G4ProcessType type = (*plist)[j]->GetProcessType();
      std::string name = (*plist)[j]->GetProcessName();
      if ((type == fElectromagnetic) && (name != "msc")) {
        processNameVector.push_back(name);
      }
    }

    MaterialMuImageType::RegionType region;
    region.SetSize(0, m.size());
    region.SetSize(1, Elist.size());
    m_MaterialMu = MaterialMuImageType::New();
    m_MaterialMu->SetRegions(region);
    m_MaterialMu->Allocate();
    itk::ImageRegionIterator< MaterialMuImageType > it(m_MaterialMu, region);
    for(unsigned int e=0; e<Elist.size(); e++) {
      for(unsigned int i=0; i<m.size(); i++) {
        G4Material * mat = m[i];
        //double d = mat->GetDensity(); // not needed
        double mu = 0;
        for (unsigned int j = 0; j < processNameVector.size(); j++) {
          // Note: the G4EmCalculator retrive the correct G4VProcess
          // (standard, Penelope, Livermore) from the processName.
          double xs =
              emCalculator->ComputeCrossSectionPerVolume(Elist[e], "gamma", processNameVector[j], mat->GetName());
          // In (length unit)^{-1} according to
          // http://www.lcsim.org/software/geant4/doxygen/html/classG4EmCalculator.html#a870d5fffaca35f6e2946da432034bd4c
          mu += xs;
        }
        it.Set(mu);
        ++it;
      }
    }

    muProbe.Stop();
    G4cout << "Computation of the mu lookup table took "
           << muProbe.GetTotal()
           << ' '
           << muProbe.GetUnit()
           << G4endl;
  }

  MaterialMuImageType::Pointer GetMaterialMuMap() { return m_MaterialMu; }

  double GetIntegralOverDetectorAndReset()
  {
    double result = 0.;
    for(int i=0; i<ITK_MAX_THREADS; i++){
      result += m_IntegralOverDetector[i];
      m_IntegralOverDetector[i] = 0.;
    }
    return result;
  }

  double GetSquaredIntegralOverDetectorAndReset()
  {
    double result = 0.;
    for(int i=0; i<ITK_MAX_THREADS; i++){
      result += m_SquaredIntegralOverDetector[i];
      m_SquaredIntegralOverDetector[i] = 0.;
    }
    return result;
  }

  void SetNumberOfPrimaries(G4int i) { m_NumberOfPrimaries = i; }
  void SetResponseDetector(GateEnergyResponseFunctor *_arg){ m_ResponseDetector = _arg; }
  void SetEnergyResolvedParameters(const double bin, const unsigned int slice)
  {
    m_EnergyResolvedBinSize = bin;
    m_EnergyResolvedSliceSize = slice;
  }

protected:
  inline void Accumulate(const rtk::ThreadIdType threadId,
                         float &output,
                         const double valueToAccumulate,
                         const double energy)
  {
    const std::ptrdiff_t offset = m_EnergyResolvedSliceSize * itk::Math::Floor<unsigned int>(energy/m_EnergyResolvedBinSize+0.5);
    *(&output+offset) += valueToAccumulate;
    m_IntegralOverDetector[threadId] += valueToAccumulate;
    m_SquaredIntegralOverDetector[threadId] += valueToAccumulate * valueToAccumulate;
  }

  VectorType                    m_VolumeSpacing;
  std::vector<double>          *m_InterpolationWeights;
  std::vector<double>          *m_EnergyWeightList;
  MaterialMuImageType::Pointer  m_MaterialMu;
  VectorType                    m_DetectorOrientationTimesPixelSurface;
  double                        m_IntegralOverDetector[ITK_MAX_THREADS];
  double                        m_SquaredIntegralOverDetector[ITK_MAX_THREADS];
  G4int                         m_NumberOfPrimaries;
  GateEnergyResponseFunctor    *m_ResponseDetector;
  std::vector<double>           m_EnergyList;
  double                        m_EnergyResolvedBinSize;
  unsigned int                  m_EnergyResolvedSliceSize;
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Most of the computation for the primary is done in this functor. After a ray
// has been cast, it loops over the energies, computes the ray line integral for
// that energy and takes the exponential of the opposite and add.
template<bool VIsEnergyResolved = false>
class PrimaryValueAccumulation:
    public VAccumulation<VIsEnergyResolved>
{
public:
  static const unsigned int Dimension = 3;
  typedef itk::Vector<double, Dimension> VectorType;

  PrimaryValueAccumulation() {}
  ~PrimaryValueAccumulation() {}

  inline void operator()( const rtk::ThreadIdType threadId,
                          const float &itkNotUsed(input),
                          float &output,
                          const double &itkNotUsed(rayCastValue),
                          const VectorType &stepInMM,
                          const VectorType &itkNotUsed(source),
                          const VectorType &sourceToPixel,
                          const VectorType &nearestPoint,
                          const VectorType &farthestPoint)
  {
    double *p = this->m_MaterialMu->GetPixelContainer()->GetBufferPointer();

    // Multiply interpolation weights by step norm in MM to convert voxel
    // intersection length to MM.
    const double stepInMMNorm = stepInMM.GetNorm();
    for(unsigned int j=0; j<this->m_InterpolationWeights[threadId].size()-1; j++)
      this->m_InterpolationWeights[threadId][j] *= stepInMMNorm;

    // The last material is the world material. One must fill the weight with
    // the length from source to nearest point and farthest point to pixel
    // point.
    VectorType worldVector = sourceToPixel + nearestPoint - farthestPoint;
    for(int i=0; i<3; i++)
      worldVector[i] *= this->m_VolumeSpacing[i];
    this->m_InterpolationWeights[threadId].back() += worldVector.GetNorm();

    // Loops over energy, multiply weights by mu, accumulate using Beer Lambert
    for(unsigned int i=0; i<this->m_EnergyWeightList->size(); i++) {
      double rayIntegral = 0.;
      for(unsigned int j=0; j<this->m_InterpolationWeights[threadId].size(); j++){
        rayIntegral += this->m_InterpolationWeights[threadId][j] * *p++;
      }

      //statistical noise added
      if(this->m_NumberOfPrimaries != 0)
      	{
        double a =vcl_exp(-rayIntegral);
        double nprimE = this->m_NumberOfPrimaries * (*this->m_EnergyWeightList)[i];
        double n = ((nprimE)?G4Poisson(nprimE*a)/nprimE:0.);
        this->Accumulate(threadId, output, n * (*this->m_EnergyWeightList)[i] * (*this->m_ResponseDetector)(this-> m_EnergyList[i] ), this->m_EnergyList[i]);
        }
      else
        this->Accumulate(threadId, output, vcl_exp(-rayIntegral) * (*this->m_EnergyWeightList)[i], this->m_EnergyList[i]);
    }

    // Reset weights for next ray in thread.
    std::fill(this->m_InterpolationWeights[threadId].begin(),
              this->m_InterpolationWeights[threadId].end(),
              0.);
  }
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
template<bool VIsEnergyResolved = false>
class ComptonValueAccumulation:
    public VAccumulation<VIsEnergyResolved>
{
public:
  static const unsigned int Dimension = 3;
  typedef itk::Vector<double, Dimension> VectorType;

  ComptonValueAccumulation() {
    // G4 data
    G4VDataSetAlgorithm* scatterInterpolation = new G4LogLogInterpolation;
    G4String scatterFile = "comp/ce-sf-";
    m_ScatterFunctionData = new G4CompositeEMDataSet( scatterInterpolation, 1., 1.);
    m_ScatterFunctionData->LoadData(scatterFile);

    m_CrossSectionHandler = new G4CrossSectionHandler;
    G4String crossSectionFile = "comp/ce-cs-";
    m_CrossSectionHandler->LoadData(crossSectionFile);
    }
  ~ComptonValueAccumulation() {
    delete m_ScatterFunctionData;
    delete m_CrossSectionHandler;
  }

  inline void operator()( const rtk::ThreadIdType threadId,
                          const float &itkNotUsed(input),
                          float &output,
                          const double &itkNotUsed(rayCastValue),
                          const VectorType &stepInMM,
                          const VectorType &itkNotUsed(source),
                          const VectorType &sourceToPixel,
                          const VectorType &nearestPoint,
                          const VectorType &farthestPoint)
  {
    // Compute ray length in world material
    // This is used to compute the length in world as well as the direction
    // of the ray in mm.
    VectorType worldVector = sourceToPixel + nearestPoint - farthestPoint;
    for(int i=0; i<3; i++)
      worldVector[i] *= this->m_VolumeSpacing[i];
    const double worldVectorNorm = worldVector.GetNorm();

    // This is taken from G4LivermoreComptonModel.cc
    double cosT = worldVector * m_Direction / worldVectorNorm;
    double x = std::sqrt(1.-cosT) * m_InvWlPhoton;// 1-cosT=2*sin(T/2)^2
    double scatteringFunction = m_ScatterFunctionData->FindValue(x,m_Z-1);

    // This is taken from GateDiffCrossSectionActor.cc and simplified
    double Eratio = 1./(1.+m_E0m*(1.-cosT));
    //double DCSKleinNishina = m_eRadiusOverCrossSectionTerm *
    //                         Eratio * Eratio *                      // DCSKleinNishinaTerm1
    //                         (Eratio + 1./Eratio - 1. + cosT*cosT); // DCSKleinNishinaTerm2
    double DCSKleinNishina = m_eRadiusOverCrossSectionTerm*Eratio*(1.+Eratio*(Eratio-1.+cosT*cosT));
    double DCScompton = DCSKleinNishina * scatteringFunction;

    // Multiply interpolation weights by step norm in MM to convert voxel
    // intersection length to MM.
    const double stepInMMNorm = stepInMM.GetNorm();
    for(unsigned int j=0; j<this->m_InterpolationWeights[threadId].size()-1; j++)
      this->m_InterpolationWeights[threadId][j] *= stepInMMNorm;

    // The last material is the world material. One must fill the weight with
    // the length from farthest point to pixel point.
    this->m_InterpolationWeights[threadId].back() = worldVectorNorm;

    const double energy = Eratio*m_Energy;
    unsigned int e = itk::Math::Round<double, double>(energy / this->m_MaterialMu->GetSpacing()[1]);
    double *p = this->m_MaterialMu->GetPixelContainer()->GetBufferPointer() +
                e * this->m_MaterialMu->GetLargestPossibleRegion().GetSize()[0];

    // Ray integral
    double rayIntegral = 0.;
    for(unsigned int j=0; j<this->m_InterpolationWeights[threadId].size(); j++)
      rayIntegral += this->m_InterpolationWeights[threadId][j] * *p++;

    // Final computation
    this->Accumulate(threadId,
                     output,
                     vcl_exp(-rayIntegral) * DCScompton * this->GetSolidAngle(sourceToPixel) * (*this->m_ResponseDetector)(energy),
                     energy);

    // Reset weights for next ray in thread.
    std::fill(this->m_InterpolationWeights[threadId].begin(),
              this->m_InterpolationWeights[threadId].end(),
              0.);
  }

  void SetDirection(const VectorType &_arg){ m_Direction = _arg; }

  void SetEnergyZAndWeight(const double  &energy, const unsigned int &Z, const double &weight) {

    m_Energy = energy;
    m_E0m = m_Energy / electron_mass_c2;
    m_InvWlPhoton = std::sqrt(0.5) * cm * m_Energy / (h_Planck * c_light); // sqrt(0.5) for trigo reasons, see comment when used

    G4double cs = m_CrossSectionHandler->FindValue(Z, energy);
    m_Z = Z;
    m_eRadiusOverCrossSectionTerm = weight * ( classic_electr_radius*classic_electr_radius) / (2.*cs);
  }

private:
  VectorType                 m_Direction;
  double                     m_Energy;
  double                     m_E0m;
  double                     m_InvWlPhoton;
  unsigned int               m_Z;
  double                     m_eRadiusOverCrossSectionTerm;

  // Compton data
  G4VEMDataSet* m_ScatterFunctionData;
  G4VCrossSectionHandler* m_CrossSectionHandler;
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
template<bool VIsEnergyResolved = false>
class RayleighValueAccumulation:
    public VAccumulation<VIsEnergyResolved>
{
public:
  static const unsigned int Dimension = 3;
  typedef itk::Vector<double, Dimension> VectorType;

  RayleighValueAccumulation() {
    // G4 data
    G4VDataSetAlgorithm* ffInterpolation = new G4LogLogInterpolation;
    G4String formFactorFile = "rayl/re-ff-";
    m_FormFactorData = new G4CompositeEMDataSet( ffInterpolation, 1., 1.);
    m_FormFactorData->LoadData(formFactorFile);

    m_CrossSectionHandler = new G4CrossSectionHandler;
    G4String crossSectionFile = "rayl/re-cs-";
    m_CrossSectionHandler->LoadData(crossSectionFile);
  }
  ~RayleighValueAccumulation() {
    delete m_FormFactorData;
    delete m_CrossSectionHandler;
  }

  inline void operator()( const rtk::ThreadIdType threadId,
                          const float &itkNotUsed(input),
                          float &output,
                          const double &itkNotUsed(rayCastValue),
                          const VectorType &stepInMM,
                          const VectorType &itkNotUsed(source),
                          const VectorType &sourceToPixel,
                          const VectorType &nearestPoint,
                          const VectorType &farthestPoint)
  {
    // Compute ray length in world material
    // This is used to compute the length in world as well as the direction
    // of the ray in mm.
    VectorType worldVector = sourceToPixel + nearestPoint - farthestPoint;
    for(int i=0; i<3; i++)
      worldVector[i] *= this->m_VolumeSpacing[i];
    const double worldVectorNorm = worldVector.GetNorm();

    // This is taken from GateDiffCrossSectionActor.cc and simplified
    double cosT = worldVector * m_Direction / worldVectorNorm;
    double DCSThomsonTerm1 = (1 + cosT * cosT);
    double DCSThomson = m_eRadiusOverCrossSectionTerm * DCSThomsonTerm1;
    double x = std::sqrt(1.-cosT) * m_InvWlPhoton;// 1-cosT=2*sin(T/2)^2
    double formFactor = m_FormFactorData->FindValue(x, m_Z-1);
    double DCSrayleigh = DCSThomson * formFactor * formFactor;

    // Multiply interpolation weights by step norm in MM to convert voxel
    // intersection length to MM.
    const double stepInMMNorm = stepInMM.GetNorm();
    for(unsigned int j=0; j<this->m_InterpolationWeights[threadId].size()-1; j++)
      this->m_InterpolationWeights[threadId][j] *= stepInMMNorm;

    // The last material is the world material. One must fill the weight with
    // the length from farthest point to pixel point.
    this->m_InterpolationWeights[threadId].back() = worldVectorNorm;

    // Ray integral
    double rayIntegral = 0.;
    for(unsigned int j=0; j<this->m_InterpolationWeights[threadId].size(); j++)
      rayIntegral += this->m_InterpolationWeights[threadId][j] * *(this->m_MaterialMuPointer+j);

    // Final computation
    this->Accumulate(threadId,
                     output,
                     vcl_exp(-rayIntegral) * DCSrayleigh * this->GetSolidAngle(sourceToPixel),
                     m_Energy);

    // Reset weights for next ray in thread.
    std::fill(this->m_InterpolationWeights[threadId].begin(),
              this->m_InterpolationWeights[threadId].end(),
              0.);
  }

  void SetDirection(const VectorType &_arg){ m_Direction = _arg; }
  void SetEnergyZAndWeight(const double  &energy, const unsigned int &Z, const double &weight) {
    unsigned int e = itk::Math::Round<double, double>(energy / this->m_MaterialMu->GetSpacing()[1]);
    m_InvWlPhoton = std::sqrt(0.5) * cm * energy / (h_Planck * c_light); // sqrt(0.5) for trigo reasons, see comment when used
    m_Energy = energy;
    m_MaterialMuPointer = this->m_MaterialMu->GetPixelContainer()->GetBufferPointer();
    m_MaterialMuPointer += e * this->m_MaterialMu->GetLargestPossibleRegion().GetSize()[0];

    G4double cs = m_CrossSectionHandler->FindValue(Z, energy);
    m_Z = Z;
    m_eRadiusOverCrossSectionTerm = weight * ( classic_electr_radius*classic_electr_radius) / (2.*cs);
  }

private:
  VectorType           m_Direction;
  double              *m_MaterialMuPointer;
  double               m_InvWlPhoton;
  double               m_Energy;
  unsigned int         m_Z;
  double               m_eRadiusOverCrossSectionTerm;

  // G4 data
  G4VEMDataSet* m_FormFactorData;
  G4VCrossSectionHandler* m_CrossSectionHandler;
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
template<bool VIsEnergyResolved = false>
class FluorescenceValueAccumulation:
    public VAccumulation<VIsEnergyResolved>
{
public:
  static const unsigned int Dimension = 3;
  typedef itk::Vector<double, Dimension> VectorType;

  FluorescenceValueAccumulation() {}
  ~FluorescenceValueAccumulation() {}

  inline void operator()( const rtk::ThreadIdType threadId,
                          const float &itkNotUsed(input),
                          float &output,
                          const double &itkNotUsed(rayCastValue),
                          const VectorType &stepInMM,
                          const VectorType &itkNotUsed(source),
                          const VectorType &sourceToPixel,
                          const VectorType &nearestPoint,
                          const VectorType &farthestPoint)
  {
    // Compute ray length in world material
    // This is used to compute the length in world as well as the direction
    // of the ray in mm.
    VectorType worldVector = sourceToPixel + nearestPoint - farthestPoint;
    for(int i=0; i<3; i++)
      worldVector[i] *= this->m_VolumeSpacing[i];
    const double worldVectorNorm = worldVector.GetNorm();

    // Multiply interpolation weights by step norm in MM to convert voxel
    // intersection length to MM.
    const double stepInMMNorm = stepInMM.GetNorm();
    for(unsigned int j=0; j<this->m_InterpolationWeights[threadId].size()-1; j++)
      this->m_InterpolationWeights[threadId][j] *= stepInMMNorm;

    // The last material is the world material. One must fill the weight with
    // the length from farthest point to pixel point.
    this->m_InterpolationWeights[threadId].back() = worldVectorNorm;

    // Ray integral
    double rayIntegral = 0.;
    for(unsigned int j=0; j<this->m_InterpolationWeights[threadId].size(); j++)
      rayIntegral += this->m_InterpolationWeights[threadId][j] * *(m_MaterialMuPointer+j);

    // Final computation
    this->Accumulate(threadId,
                     output,
                     m_Weight * vcl_exp(-rayIntegral)*this->GetSolidAngle(sourceToPixel)/(4*itk::Math::pi),
                     m_Energy);

    // Reset weights for next ray in thread.
    std::fill(this->m_InterpolationWeights[threadId].begin(),
              this->m_InterpolationWeights[threadId].end(),
              0.);
  }

  void SetDirection(const VectorType &itkNotUsed(_arg)){}
  void SetEnergyZAndWeight(const double &energy, const unsigned int &itkNotUsed(Z), const double &weight) {
    unsigned int e = itk::Math::Round<double, double>(energy / this->m_MaterialMu->GetSpacing()[1]);
    m_Weight = weight;
    m_Energy = energy;
    m_MaterialMuPointer = this->m_MaterialMu->GetPixelContainer()->GetBufferPointer();
    m_MaterialMuPointer += e * this->m_MaterialMu->GetLargestPossibleRegion().GetSize()[0];
  }

private:
  double    *m_MaterialMuPointer;
  double     m_Weight;
  double     m_Energy;
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
template< class TInput1, class TInput2 = TInput1, class TOutput = TInput1 >
class Attenuation
{
  public:
  Attenuation() {}
  ~Attenuation() {}
  bool operator!=(const Attenuation &) const
  {
    return false;
  }

  bool operator==(const Attenuation & other) const
  {
    return !( *this != other );
  }

  inline TOutput operator()(const TInput1 A,const TInput2 B) const
  {
    //Calculating attenuation image (-log(primaryImage/flatFieldImage))
    return (TOutput)(-log(A/B));
  }
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
template< class TInput1, class TInput2 = TInput1, class TOutput = TInput1 >
class Chetty
{
  public:
  Chetty() {}
  ~Chetty() {}
  bool operator!=(const Chetty &) const
  {
    return false;
  }

  bool operator==(const Chetty & other) const
  {
    return !( *this != other );
  }

  void SetN(double N) {m_invN = 1/N; m_invNm1 = 1/(N-1);}

  inline TOutput operator()(const TInput1 sum,const TInput2 squaredSum) const
  {
    // Chetty, IJROBP, 2006, p1250, eq 2
    return sqrt(m_invNm1*(squaredSum*m_invN-pow(sum * m_invN, 2.)))/(sum*m_invN);
  }
private:
  double m_invN;
  double m_invNm1;
};
//-----------------------------------------------------------------------------

}

#endif // GATEFIXEDFORCEDDETECTIONACTORFUNCTORS_HH
