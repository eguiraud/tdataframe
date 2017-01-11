#ifndef ROOT_TDATAFRAME
#define ROOT_TDATAFRAME

#include "TBranchElement.h"
#include "TDirectory.h"
#include "TH1F.h" // For Histo actions
#include "TTreeReader.h"
#include "TTreeReaderValue.h"
#include "TROOT.h" // IsImplicitMTEnabled, GetImplicitMTPoolSize
#include "ROOT/TSpinMutex.hxx"
#include "ROOT/TTreeProcessor.hxx"

#include <algorithm> // std::find
#include <array>
#include <map>
#include <memory>
#include <list>
#include <string>
#include <thread>
#include <type_traits> // std::decay
#include <typeinfo>
#include <utility> // std::move
#include <vector>

// Meta programming utilities, perhaps to be moved in core/foundation
namespace ROOT {
   namespace Internal {
      template<typename...Types>
      struct TypeList {
         static constexpr std::size_t size = sizeof...(Types);
      };

      // extract parameter types from a callable object
      template<typename T>
      struct FunctionTraits : public FunctionTraits<decltype(&T::operator())> {};

      // lambdas and std::function
      template<typename R, typename T, typename... Args>
      struct FunctionTraits<R(T::*)(Args...) const> {
         using ArgTypes = TypeList<typename std::decay<Args>::type...>;
         using ArgTypesNoDecay = TypeList<Args...>;
         using RetType = R;
      };

      // mutable lambdas and functor classes
      template<typename R, typename T, typename... Args>
      struct FunctionTraits<R(T::*)(Args...)> {
         using ArgTypes = TypeList<typename std::decay<Args>::type...>;
         using ArgTypesNoDecay = TypeList<Args...>;
         using RetType = R;
      };

      // free functions
      template<typename R, typename... Args>
      struct FunctionTraits<R(*)(Args...)> {
         using ArgTypes = TypeList<typename std::decay<Args>::type...>;
         using ArgTypesNoDecay = TypeList<Args...>;
         using RetType = R;
      };

      // remove first type from TypeList
      template<typename>
      struct RemoveFirst {};

      template<typename T, typename...Args>
      struct RemoveFirst<TypeList<T, Args...>> {
         using Types = TypeList<Args...>;
      };

      // returned wrapper around f that adds the unsigned slot parameter
      template<typename R, typename F, typename...Args>
      std::function<R(unsigned, Args...)> AddSlotParameter(F f, TypeList<Args...>) {
         return [f] (unsigned, Args...a) -> R { return f(a...); };
      }

      // compile-time integer sequence generator
      // e.g. calling gens<3>::type() instantiates a seq<0,1,2>
      template<int ...>
      struct seq {};

      template<int N, int ...S>
      struct gens : gens<N-1, N-1, S...> {};

      template<int ...S>
      struct gens<0, S...>{
         using type = seq<S...>;
      };

      // IsContainer check for type T
      template<typename T>
      struct IsContainer{
         using test_type = typename std::remove_const<T>::type;

         template<typename A>
         static constexpr bool test (
            A * pt,
            A const * cpt = nullptr,
            decltype(pt->begin()) * = nullptr,
            decltype(pt->end()) * = nullptr,
            decltype(cpt->begin()) * = nullptr,
            decltype(cpt->end()) * = nullptr,
            typename A::iterator * pi = nullptr,
            typename A::const_iterator * pci = nullptr,
            typename A::value_type * pv = nullptr)
         {
            using iterator =  typename A::iterator;
            using const_iterator = typename A::const_iterator;
            using value_type = typename A::value_type;
            return  std::is_same<test_type, std::vector<bool>>::value ||
                  (std::is_same<decltype(pt->begin()),iterator>::value &&
                     std::is_same<decltype(pt->end()),iterator>::value &&
                     std::is_same<decltype(cpt->begin()),const_iterator>::value &&
                     std::is_same<decltype(cpt->end()),const_iterator>::value &&
                     std::is_same<decltype(**pi),value_type &>::value &&
                     std::is_same<decltype(**pci),value_type const &>::value);
         }

         template<typename A>
         static constexpr bool test(...) {
            return false;
         }

         static const bool value = test<test_type>(nullptr);
      };
   }
}

// Result pointer: can move below?
namespace ROOT {

   using BranchList = std::vector<std::string>;

   // Fwd declarations
   namespace Details {
      class TDataFrameImpl;
   }

   // Smart pointer for the return type of actions
   class TActionResultPtrBase {
      friend class Details::TDataFrameImpl;
      std::weak_ptr<Details::TDataFrameImpl> fFirstData;
      std::shared_ptr<bool> fReadyPtr = std::make_shared<bool>(false);
      void SetReady() {*fReadyPtr = true;}
   protected:
      bool IsReady() {return *fReadyPtr;}
      void TriggerRun();
   public:
      TActionResultPtrBase(std::weak_ptr<Details::TDataFrameImpl> firstData);
      virtual ~TActionResultPtrBase() {
      }
   };

   template<typename T>
   class TActionResultPtr : public TActionResultPtrBase{
      std::shared_ptr<T> fObjPtr;
   public:
      TActionResultPtr(std::shared_ptr<T> objPtr, std::weak_ptr<Details::TDataFrameImpl> firstData): TActionResultPtrBase(firstData), fObjPtr(objPtr){}
      T *get() {
         if (!IsReady()) TriggerRun();
         return fObjPtr.get();
      }
      T& operator*() { return *get(); }
      T *operator->() { return get(); }
      T *getUnchecked() { return fObjPtr.get(); }
   };
}

// Internal classes
namespace ROOT {

   namespace Details {
      class TDataFrameImpl;
   }

   namespace Internal {
      using namespace Details;
      using TVBPtr = std::shared_ptr<TTreeReaderValueBase>;
      using TVBVec = std::vector<TVBPtr>;

      template<int... S, typename... BranchTypes>
      TVBVec BuildReaderValues(TTreeReader& r, const BranchList& bl,
                              const BranchList& tmpbl, TypeList<BranchTypes...>, seq<S...>)
      {
         // isTmpBranch has length bl.size(). Elements are true if the corresponding
         // branch is a "fake" branch created with AddBranch, false if they are
         // actual branches present in the TTree.
         std::array<bool, sizeof...(S)> isTmpBranch;
         for(unsigned i = 0; i < isTmpBranch.size(); ++i)
            isTmpBranch[i] = std::find(tmpbl.begin(), tmpbl.end(), bl.at(i)) != tmpbl.end();

         // Build vector of pointers to TTreeReaderValueBase.
         // tvb[i] points to a TTreeReaderValue specialized for the i-th BranchType,
         // corresponding to the i-th branch in bl
         // For temporary branches (declared with AddBranch) a nullptr is created instead
         // S is expected to be a sequence of sizeof...(BranchTypes) integers
         TVBVec tvb{
            isTmpBranch[S] ?
            nullptr :
            std::make_shared<TTreeReaderValue<BranchTypes>>(r, bl.at(S).c_str())... }; // "..." expands BranchTypes and S simultaneously

         return tvb;
      }


      template<typename Filter>
      void CheckFilter(Filter f) {
         using FilterRetType = typename FunctionTraits<Filter>::RetType;
         static_assert(std::is_same<FilterRetType, bool>::value,
                     "filter functions must return a bool");
      }


      template<typename F>
      const BranchList& PickBranchList(F f, const BranchList& bl, const BranchList& defBl) {
         // return local BranchList or default BranchList according to which one
         // should be used
         auto nArgs = FunctionTraits<F>::ArgTypes::size;
         bool useDefBl = false;
         if(nArgs != bl.size()) {
            if(bl.size() == 0 && nArgs == defBl.size()) {
               useDefBl = true;
            } else {
               auto msg = "mismatch between number of filter arguments ("
                           + std::to_string(nArgs) + ") and number of branches ("
                           + std::to_string(bl.size()?:defBl.size()) + ")";
               throw std::runtime_error(msg);
            }
         }

         return useDefBl ? defBl : bl;
      }


      class TDataFrameActionBase {
      public:
         virtual ~TDataFrameActionBase() { }
         virtual void Run(unsigned slot, int entry) = 0;
         virtual void BuildReaderValues(TTreeReader& r, unsigned slot) = 0;
         virtual void CreateSlots(unsigned nSlots) = 0;
      };
      using ActionBasePtr = std::shared_ptr<TDataFrameActionBase>;
      using ActionBaseVec = std::vector<ActionBasePtr>;

      // Forward declarations
      template<int S, typename T>
      T& GetBranchValue(TVBPtr& readerValues, unsigned slot, int entry, const std::string& branch, std::weak_ptr<Details::TDataFrameImpl> df);


      template<typename F, typename PrevDataFrame>
      class TDataFrameAction : public TDataFrameActionBase {
         using BranchTypes = typename RemoveFirst<typename FunctionTraits<F>::ArgTypes>::Types;
         using TypeInd = typename gens<BranchTypes::size>::type;

      public:
         TDataFrameAction(F f, const BranchList& bl, std::weak_ptr<PrevDataFrame> pd)
            : fAction(f), fBranches(bl), fTmpBranches(pd.lock()->fTmpBranches),
            fPrevData(pd.lock().get()), fFirstData(pd.lock()->fFirstData) { }

         TDataFrameAction(const TDataFrameAction&) = delete;

         void Run(unsigned slot, int entry) {
            // check if entry passes all filters
            if(CheckFilters(slot, entry))
               ExecuteAction(slot, entry);
         }

         bool CheckFilters(unsigned slot, int entry) {
            // start the recursive chain of CheckFilters calls
            return fPrevData->CheckFilters(slot, entry);
         }

         void ExecuteAction(unsigned slot, int entry) {
            ExecuteActionHelper(slot, entry, TypeInd(), BranchTypes());
         }

         void CreateSlots(unsigned nSlots) {
            fReaderValues.resize(nSlots);
         }

         void BuildReaderValues(TTreeReader& r, unsigned slot) {
            fReaderValues[slot] = ROOT::Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes(), TypeInd());
         }

         template<int... S, typename... BranchTypes>
         void ExecuteActionHelper(unsigned slot, int entry, seq<S...>, TypeList<BranchTypes...>) {
            // Take each pointer in tvb, cast it to a pointer to the
            // correct specialization of TTreeReaderValue, and get its content.
            // S expands to a sequence of integers 0 to sizeof...(types)-1
            // S and types are expanded simultaneously by "..."
            fAction(slot, GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...);
         }

         F fAction;
         const BranchList fBranches;
         const BranchList fTmpBranches;
         PrevDataFrame* fPrevData;
         std::weak_ptr<Details::TDataFrameImpl> fFirstData;
         std::vector<TVBVec> fReaderValues;
      };


      namespace Operations {
         class FillOperation{
            TH1F* fResultHist;
            std::vector<TH1F> fHists;
         public:
            FillOperation(TH1F* h, unsigned nSlots)
               : fResultHist(h), fHists(nSlots) {}
            template<typename T, typename std::enable_if<!IsContainer<T>::value, int>::type = 0>
            void Exec(T v, unsigned ) {
               fResultHist->Fill(v);
            }
            template<typename T, typename std::enable_if<IsContainer<T>::value, int>::type = 0>
            void Exec(const T&  vs, unsigned ){
               for(auto&& v : vs)
                  fResultHist->Fill(v);
            }
            ~FillOperation() {
               // FIXME merge fHists in fResultHist
               //fHists[0].Copy(*fResultHist);
            }
         };

         class MinOperation{
            double* fResultMin;
            std::vector<double> fMins;
         public:
            MinOperation(double* minVPtr, unsigned nSlots)
               : fResultMin(minVPtr), fMins(nSlots, std::numeric_limits<double>::max()) {}
            template<typename T, typename std::enable_if<!IsContainer<T>::value, int>::type = 0>
            void Exec(T v, unsigned slot){
               fMins[slot] = std::min((double)v, fMins[slot]);
            }
            template<typename T, typename std::enable_if<IsContainer<T>::value, int>::type = 0>
            void Exec(const T&  vs, unsigned slot){
               for(auto&& v : vs)
                  fMins[slot] = std::min((double)v, fMins[slot]);
            }
            ~MinOperation() {
               *fResultMin = std::numeric_limits<double>::max();
               for(auto& m : fMins)
                  *fResultMin = std::min(m, *fResultMin);
            }
         };

         class MaxOperation{
            double* fResultMax;
            std::vector<double> fMaxs;
         public:
            MaxOperation(double* maxVPtr, unsigned nSlots)
               : fResultMax(maxVPtr), fMaxs(nSlots, std::numeric_limits<double>::min()) {}
            template<typename T, typename std::enable_if<!IsContainer<T>::value, int>::type = 0>
            void Exec(T v, unsigned slot) {
               fMaxs[slot] = std::max((double)v, fMaxs[slot]);
            }
            template<typename T, typename std::enable_if<IsContainer<T>::value, int>::type = 0>
            void Exec(const T&  vs, unsigned slot) {
               for(auto&& v : vs)
                  fMaxs[slot] = std::max((double)v, fMaxs[slot]);
            }
            ~MaxOperation() {
               *fResultMax = std::numeric_limits<double>::min();
               for(auto& m : fMaxs) {
                  *fResultMax = std::max(m, *fResultMax);
               }
            }
         };

         class MeanOperation {
            double* fResultMean;
            std::vector<unsigned long> fCounts;
            std::vector<double> fMeans;
         public:
            MeanOperation(double* meanVPtr, unsigned nSlots)
               : fResultMean(meanVPtr), fCounts(nSlots, 0), fMeans(nSlots, 0) {}
            template<typename T, typename std::enable_if<!IsContainer<T>::value, int>::type = 0>
            void Cumulate(T v, unsigned slot) {
               fMeans[slot] += v;
               fCounts[slot] += 1;
            }
            template<typename T, typename std::enable_if<IsContainer<T>::value, int>::type = 0>
            void Cumulate(const T& vs, unsigned slot) {
               for(auto&& v : vs) {
                  fMeans[slot] += v;
                  fCounts[slot] += 1;
               }
            }
            ~MeanOperation() {
               double sumOfMeans = 0;
               for(unsigned i = 0; i < fMeans.size(); ++i)
                  sumOfMeans += fMeans[i] / fCounts[i];
               *fResultMean = sumOfMeans / fMeans.size();
            }
         };
      }


      enum class EActionType : short {
         kHisto1D,
         kMin,
         kMax,
         kMean
      };

   } // end NS Internal

   namespace Details {
      // forward declarations for TDataFrameInterface
      template<typename F, typename PrevData>
      class TDataFrameFilter;
      template<typename F, typename PrevData>
      class TDataFrameBranch;
      class TDataFrameImpl;
   }

  template<typename Derived>
   class TDataFrameInterface {
   public:
      TDataFrameInterface() = delete;
      TDataFrameInterface(std::shared_ptr<Derived> derived) : fDerivedPtr(derived) { }
      TDataFrameInterface(const std::string& treeName,
                          TDirectory* dirPtr,
                          const BranchList& defaultBranches = {});
      TDataFrameInterface(TTree& tree, const BranchList& defaultBranches = {});

      template<typename F>
      TDataFrameInterface<Details::TDataFrameFilter<F, Derived>> Filter(F f, const BranchList& bl = {}) {
         ROOT::Internal::CheckFilter(f);
         const BranchList& defBl = fDerivedPtr->GetDataFrame().lock()->GetDefaultBranches();
         const BranchList& actualBl = Internal::PickBranchList(f, bl, defBl);
         using DFF = Details::TDataFrameFilter<F, Derived>;
         std::shared_ptr<DFF> FilterPtr(new DFF(f, actualBl, fDerivedPtr));
         TDataFrameInterface<DFF> tdf_f(FilterPtr);
         Book(FilterPtr);
         return tdf_f;
      }

      template<typename F>
      TDataFrameInterface<Details::TDataFrameBranch<F, Derived>> AddBranch(const std::string& name, F expression, const BranchList& bl = {}) {
         const BranchList& defBl = fDerivedPtr->GetDataFrame().lock()->GetDefaultBranches();
         const BranchList& actualBl = Internal::PickBranchList(expression, bl, defBl);
         using DFB = Details::TDataFrameBranch<F, Derived>;
         auto BranchPtr = std::shared_ptr<DFB>(new DFB(name, expression, actualBl, fDerivedPtr));
         TDataFrameInterface<DFB> tdf_b(BranchPtr);
         Book(BranchPtr);
         return tdf_b;
      }

      template<typename F>
      void Foreach(F f, const BranchList& bl = {}) {
         GetDataFrameChecked();
         const BranchList& defBl = fDerivedPtr->GetDataFrame().lock()->GetDefaultBranches();
         const BranchList& actualBl = Internal::PickBranchList(f, bl, defBl);
         using ArgTypes = typename Internal::FunctionTraits<decltype(f)>::ArgTypesNoDecay;
         using RetType = typename Internal::FunctionTraits<decltype(f)>::RetType;
         auto fWithSlot = Internal::AddSlotParameter<RetType>(f, ArgTypes());
         using DFA = Internal::TDataFrameAction<decltype(fWithSlot), Derived>;
         Book(std::shared_ptr<DFA>(new DFA(fWithSlot, actualBl, fDerivedPtr)));
         fDerivedPtr->GetDataFrame().lock()->Run();
      }

      TActionResultPtr<unsigned> Count() {
         TActionResultPtr<unsigned> c (std::make_shared<unsigned>(0), GetDataFrameChecked());
         auto cPtr = c.getUnchecked();
         auto countAction = [cPtr](unsigned slot /*TODO use it*/) -> void { (*cPtr)++; };
         BranchList bl = {};
         using DFA = Internal::TDataFrameAction<decltype(countAction), Derived>;
         Book(std::shared_ptr<DFA>(new DFA(countAction, bl, fDerivedPtr)));
         return c;
      }

      template<typename T, typename COLL = std::list<T>>
      TActionResultPtr<COLL> Get(const std::string& branchName = "") {
         auto theBranchName (branchName);
         GetDefaultBranchName(theBranchName, "get the values of the branch");
         TActionResultPtr<COLL> values (std::make_shared<COLL>(), GetDataFrameChecked());
         auto valuesPtr = values.getUnchecked();
         auto getAction = [valuesPtr](unsigned slot /*TODO use it*/, const T& v) { valuesPtr->emplace_back(v); };
         BranchList bl = {theBranchName};
         using DFA = Internal::TDataFrameAction<decltype(getAction), Derived>;
         Book(std::shared_ptr<DFA>(new DFA(getAction, bl, fDerivedPtr)));
         return values;
      }

      template<typename T = double>
      TActionResultPtr<TH1F> Histo(const std::string& branchName, const TH1F& model) {
         auto theBranchName (branchName);
         GetDefaultBranchName(theBranchName, "fill the histogram");
         auto h = std::make_shared<TH1F>(model);
         return CreateAction<T, Internal::EActionType::kHisto1D>(theBranchName,h);
      }

      template<typename T = double>
      TActionResultPtr<TH1F> Histo(const std::string& branchName = "", int nBins = 128, double minVal = 0., double maxVal = 0.) {
         auto theBranchName (branchName);
         GetDefaultBranchName(theBranchName, "fill the histogram");
         auto h = std::make_shared<TH1F>("","",nBins, minVal, maxVal);
         return CreateAction<T, Internal::EActionType::kHisto1D>(theBranchName,h);
      }

      template<typename T = double>
      TActionResultPtr<double> Min(const std::string& branchName = "") {
         auto theBranchName (branchName);
         GetDefaultBranchName(theBranchName, "calculate the minumum");
         auto minV = std::make_shared<T>(std::numeric_limits<T>::max());
         return CreateAction<T, Internal::EActionType::kMin>(theBranchName,minV);
      }

      template<typename T = double>
      TActionResultPtr<double> Max(const std::string& branchName = "") {
         auto theBranchName (branchName);
         GetDefaultBranchName(theBranchName, "calculate the maximum");
         auto maxV = std::make_shared<T>(std::numeric_limits<T>::min());
         return CreateAction<T, Internal::EActionType::kMax>(theBranchName,maxV);
      }

      template<typename T = double>
      TActionResultPtr<double> Mean(const std::string& branchName = "") {
         auto theBranchName (branchName);
         GetDefaultBranchName(theBranchName, "calculate the mean");
         auto meanV = std::make_shared<T>(0);
         return CreateAction<T, Internal::EActionType::kMean>(theBranchName,meanV);
      }

   private:

      /// Get the TDataFrameImpl if reachable. If not, throw.
      std::weak_ptr<Details::TDataFrameImpl> GetDataFrameChecked() {
         auto df = fDerivedPtr->GetDataFrame();
         if (df.expired()) {
            throw std::runtime_error("The main TDataFrame is not reachable: did it go out of scope?");
         }
         return df;
      }

      void GetDefaultBranchName(std::string& theBranchName, const std::string& actionNameForErr) {
         if (theBranchName.empty()) {
            // Try the default branch if possible
            const BranchList& defBl = fDerivedPtr->GetDataFrame().lock()->GetDefaultBranches();
            if (defBl.size() == 1) {
               theBranchName = defBl[0];
            } else {
               std::string msg ("No branch in input to ");
               msg += actionNameForErr;
               msg +=  " and more than one default branch.";
               throw std::runtime_error(msg);
            }
         }
      }

      template<typename BranchType, typename ActionResultType, enum Internal::EActionType, typename ThisType>
      struct SimpleAction{ };

      template<typename BranchType, typename ThisType>
      struct SimpleAction<BranchType, TH1F, Internal::EActionType::kHisto1D, ThisType> {
         static TActionResultPtr<TH1F> BuildAndBook(ThisType thisFrame, const std::string& theBranchName, std::shared_ptr<TH1F> h, unsigned nSlots) {
            auto fillOp = std::make_shared<Internal::Operations::FillOperation>(h.get(), nSlots);
            auto fillLambda = [fillOp](unsigned slot, const BranchType& v) mutable { fillOp->Exec(v, slot); };
            BranchList bl = {theBranchName};
            using DFA = Internal::TDataFrameAction<decltype(fillLambda), Derived>;
            thisFrame->Book(std::shared_ptr<DFA>(new DFA(fillLambda, bl, thisFrame->fDerivedPtr)));
            return TActionResultPtr<TH1F>(h, thisFrame->GetDataFrameChecked());
         }
      };

      template<typename BranchType, typename ThisType, typename ActionResultType>
      struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMin, ThisType> {
         static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string& theBranchName, std::shared_ptr<ActionResultType> minV, unsigned nSlots) {
            auto minOp = std::make_shared<Internal::Operations::MinOperation>(minV.get(), nSlots);
            auto minOpLambda = [minOp](unsigned slot, const BranchType& v) mutable { minOp->Exec(v, slot); };
            BranchList bl = {theBranchName};
            using DFA = Internal::TDataFrameAction<decltype(minOpLambda), Derived>;
            thisFrame->Book(std::shared_ptr<DFA>(new DFA(minOpLambda, bl, thisFrame->fDerivedPtr)));
            return TActionResultPtr<ActionResultType>(minV, thisFrame->GetDataFrameChecked());
         }
      };

      template<typename BranchType, typename ThisType, typename ActionResultType>
      struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMax, ThisType> {
         static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string& theBranchName, std::shared_ptr<ActionResultType> maxV, unsigned nSlots) {
            auto maxOp = std::make_shared<Internal::Operations::MaxOperation>(maxV.get(), nSlots);
            auto maxOpLambda = [maxOp](unsigned slot, const BranchType& v) mutable { maxOp->Exec(v, slot); };
            BranchList bl = {theBranchName};
            using DFA = Internal::TDataFrameAction<decltype(maxOpLambda), Derived>;
            thisFrame->Book(std::shared_ptr<DFA>(new DFA(maxOpLambda, bl, thisFrame->fDerivedPtr)));
            return TActionResultPtr<ActionResultType>(maxV, thisFrame->GetDataFrameChecked());
         }
      };

      template<typename BranchType, typename ThisType, typename ActionResultType>
      struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMean, ThisType> {
         static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string& theBranchName, std::shared_ptr<ActionResultType> meanV, unsigned nSlots) {
            auto meanOp = std::make_shared<Internal::Operations::MeanOperation>(meanV.get(), nSlots);
            auto meanOpLambda = [meanOp](unsigned slot, const BranchType& v) mutable { meanOp->Cumulate(v, slot);};
            BranchList bl = {theBranchName};
            using DFA = Internal::TDataFrameAction<decltype(meanOpLambda), Derived>;
            thisFrame->Book(std::shared_ptr<DFA>(new DFA(meanOpLambda, bl, thisFrame->fDerivedPtr)));
            return TActionResultPtr<ActionResultType>(meanV, thisFrame->GetDataFrameChecked());
         }
      };

      template<typename BranchType, Internal::EActionType ActionType, typename ActionResultType>
      TActionResultPtr<ActionResultType> CreateAction(const std::string& theBranchName, std::shared_ptr<ActionResultType> r) {
         // More types can be added at will at the cost of some compilation time and size of binaries.
         using ART = ActionResultType;
         using TT = decltype(this);
         const auto AT = ActionType;
         auto df = GetDataFrameChecked();
         auto tree = (TTree*) df.lock()->GetDirectory()->Get(df.lock()->GetTreeName().c_str());
         auto branch = tree->GetBranch(theBranchName.c_str());
         unsigned nSlots = 1;
   #ifdef R__USE_IMT
         if(ROOT::IsImplicitMTEnabled())
            nSlots = ROOT::GetImplicitMTPoolSize();
   #endif // R__USE_IMT
         if(!branch) {
            // temporary branch
            const auto& type_id = df.lock()->GetBookedBranch(theBranchName).GetTypeId();
            if (type_id == typeid(char)) { return SimpleAction<char, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (type_id == typeid(int)) { return SimpleAction<int, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (type_id == typeid(double)) { return SimpleAction<double, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (type_id == typeid(bool)) { return SimpleAction<bool, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (type_id == typeid(std::vector<double>)) { return SimpleAction<std::vector<double>, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (type_id == typeid(std::vector<float>)) { return SimpleAction<std::vector<float>, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
         }
         // real branch
         auto branchEl = dynamic_cast<TBranchElement*>(branch);
         if (!branchEl) { // This is a fundamental type
            auto title = branch->GetTitle();
            auto typeCode = title[strlen(title)-1];
            if (typeCode == 'B') { return SimpleAction<char, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            // else if (typeCode == 'b') { return SimpleAction<unsigned char, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            // else if (typeCode == 'S') { return SimpleAction<short, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
            // else if (typeCode == 's') { return SimpleAction<unsigned short, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (typeCode == 'I') { return SimpleAction<int, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            // else if (typeCode == 'i') { return SimpleAction<unsigned int, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            // else if (typeCode == 'F') { return SimpleAction<float, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (typeCode == 'D') { return SimpleAction<double, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            // else if (typeCode == 'L') { return SimpleAction<long int, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            // else if (typeCode == 'l') { return SimpleAction<unsigned long int, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (typeCode == 'O') { return SimpleAction<bool, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
         } else {
            std::string typeName = branchEl->GetTypeName();
            if (typeName == "vector<double>") { return SimpleAction<std::vector<double>, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
            else if (typeName == "vector<float>") { return SimpleAction<std::vector<float>, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots); }
         }
         return SimpleAction<BranchType, ART, AT, TT>::BuildAndBook(this, theBranchName, r, nSlots);
      }
      template<typename T>
      void Book(std::shared_ptr<T> ptr) {
         fDerivedPtr->Book(ptr);
      }
      std::shared_ptr<Derived> fDerivedPtr;
   };

   using TDataFrame = TDataFrameInterface<ROOT::Details::TDataFrameImpl>;

   namespace Details {

      class TDataFrameBranchBase {
      public:
         virtual ~TDataFrameBranchBase() { }
         virtual void BuildReaderValues(TTreeReader& r, unsigned slot) = 0;
         virtual void CreateSlots(unsigned nSlots) = 0;
         virtual std::string GetName() const = 0;
         virtual void* GetValue(unsigned slot, int entry) = 0;
         virtual const std::type_info& GetTypeId() const = 0;
      };
      using TmpBranchBasePtr = std::shared_ptr<TDataFrameBranchBase>;
      using TmpBranchBaseVec = std::vector<TmpBranchBasePtr>;

      template<typename F, typename PrevData>
      class TDataFrameBranch : public TDataFrameBranchBase {
         using BranchTypes = typename Internal::FunctionTraits<F>::ArgTypes;
         using TypeInd = typename Internal::gens<BranchTypes::size>::type;
         using RetType = typename Internal::FunctionTraits<F>::RetType;

      public:
         TDataFrameBranch(const std::string& name, F expression, const BranchList& bl, std::weak_ptr<PrevData> pd)
            : fName(name), fExpression(expression), fBranches(bl), fTmpBranches(pd.lock()->fTmpBranches),
            fFirstData(pd.lock()->fFirstData), fPrevData(pd.lock().get()) {
            fTmpBranches.emplace_back(name);
         }

         TDataFrameBranch(const TDataFrameBranch&) = delete;

         std::weak_ptr<TDataFrameImpl> GetDataFrame() const {
            return fFirstData;
         }

         void BuildReaderValues(TTreeReader& r, unsigned slot) {
            fReaderValues[slot] = Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes(), TypeInd());
         }

         void* GetValue(unsigned slot, int entry) {
            if(entry != fLastCheckedEntry[slot]) {
               // evaluate this filter, cache the result
               auto newValuePtr = GetValueHelper(BranchTypes(), TypeInd(), slot, entry);
               fLastResultPtr[slot] = newValuePtr;
               fLastCheckedEntry[slot] = entry;
            }
            return static_cast<void*>(fLastResultPtr[slot].get());
         }

               const std::type_info& GetTypeId() const {
                  return typeid(RetType);
               }

         template<typename T>
         void Book(std::shared_ptr<T> ptr);

         void CreateSlots(unsigned nSlots) {
            fReaderValues.resize(nSlots);
            fLastCheckedEntry.resize(nSlots);
            fLastResultPtr.resize(nSlots);
         }

         bool CheckFilters(unsigned slot, int entry) {
            // dummy call: it just forwards to the previous object in the chain
            return fPrevData->CheckFilters(slot, entry);
         }

         std::string GetName() const { return fName; }

         template<int...S, typename...BranchTypes>
         std::shared_ptr<RetType> GetValueHelper(Internal::TypeList<BranchTypes...>, Internal::seq<S...>, unsigned slot, int entry) {
            auto valuePtr = std::shared_ptr<RetType>(
               new RetType(
                  fExpression(
                     Internal::GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...
                  )
               )
            );
            return valuePtr;
         }

         const std::string fName;
         F fExpression;
         const BranchList fBranches;
         BranchList fTmpBranches;
         std::vector<ROOT::Internal::TVBVec> fReaderValues;
         std::vector<std::shared_ptr<RetType>> fLastResultPtr;
         std::weak_ptr<TDataFrameImpl> fFirstData;
         PrevData* fPrevData;
         std::vector<int> fLastCheckedEntry = {-1};
      };

      class TDataFrameFilterBase {
      public:
         virtual ~TDataFrameFilterBase() { }
         virtual void BuildReaderValues(TTreeReader& r, unsigned slot) = 0;
         virtual void CreateSlots(unsigned nSlots) = 0;
      };
      using FilterBasePtr = std::shared_ptr<TDataFrameFilterBase>;
      using FilterBaseVec = std::vector<FilterBasePtr>;

      template<typename FilterF, typename PrevDataFrame>
      class TDataFrameFilter : public TDataFrameFilterBase {
         using BranchTypes = typename Internal::FunctionTraits<FilterF>::ArgTypes;
         using TypeInd = typename Internal::gens<BranchTypes::size>::type;

      public:
         TDataFrameFilter(FilterF f, const BranchList& bl, std::weak_ptr<PrevDataFrame> pd)
            : fFilter(f), fBranches(bl), fTmpBranches(pd.lock()->fTmpBranches), fPrevData(pd.lock().get()),
            fFirstData(pd.lock()->fFirstData) { }

         std::weak_ptr<TDataFrameImpl> GetDataFrame() const {
            return fFirstData;
         }

         TDataFrameFilter(const TDataFrameFilter&) = delete;

         bool CheckFilters(unsigned slot, int entry) {
            if(entry != fLastCheckedEntry[slot]) {
               if(!fPrevData->CheckFilters(slot, entry)) {
                  // a filter upstream returned false, cache the result
                  fLastResult[slot] = false;
               } else {
                  // evaluate this filter, cache the result
                  fLastResult[slot] = CheckFilterHelper(BranchTypes(), TypeInd(), slot, entry);
               }
               fLastCheckedEntry[slot] = entry;
            }
            return fLastResult[slot];
         }

         template<int... S, typename... BranchTypes>
         bool CheckFilterHelper(Internal::TypeList<BranchTypes...>, Internal::seq<S...>, unsigned slot, int entry) {
            // Take each pointer in tvb, cast it to a pointer to the
            // correct specialization of TTreeReaderValue, and get its content.
            // S expands to a sequence of integers 0 to sizeof...(types)-1
            // S and types are expanded simultaneously by "..."
            return fFilter(Internal::GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData) ...);
         }

         void BuildReaderValues(TTreeReader& r, unsigned slot) {
            fReaderValues[slot] = Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes(), TypeInd());
         }

         template<typename T>
         void Book(std::shared_ptr<T> ptr);

         void CreateSlots(unsigned nSlots) {
            fReaderValues.resize(nSlots);
            fLastCheckedEntry.resize(nSlots);
            fLastResult.resize(nSlots);
         }

         FilterF fFilter;
         const BranchList fBranches;
         const BranchList fTmpBranches;
         PrevDataFrame* fPrevData;
         std::weak_ptr<TDataFrameImpl> fFirstData;
         std::vector<Internal::TVBVec> fReaderValues = {};
         std::vector<int> fLastCheckedEntry = {-1};
         std::vector<bool> fLastResult = {true};
      };

      class TDataFrameImpl : public std::enable_shared_from_this<TDataFrameImpl>{
      public:
         TDataFrameImpl(const std::string& treeName, TDirectory* dirPtr, const BranchList& defaultBranches = {})
            : fTreeName(treeName), fDirPtr(dirPtr),
            fDefaultBranches(defaultBranches) { }

         TDataFrameImpl(TTree& tree, const BranchList& defaultBranches = {})
            : fTree(&tree),
            fDefaultBranches(defaultBranches) { }


         TDataFrameImpl(const TDataFrameImpl&) = delete;

         void Run() {
#ifdef R__USE_IMT
            if(ROOT::IsImplicitMTEnabled()) {
               const auto fileName = fTree ? static_cast<TFile*>(fTree->GetCurrentFile())->GetName() : fDirPtr->GetName();
               const std::string treeName = fTree ? fTree->GetName() : fTreeName;
               ROOT::TTreeProcessor tp(fileName, treeName);
               ROOT::TSpinMutex slotMutex;
               std::map<std::thread::id, unsigned int> slotMap;
               unsigned globalSlotIndex = 0;
               CreateSlots(ROOT::GetImplicitMTPoolSize());
               tp.Process([this, &slotMutex, &globalSlotIndex, &slotMap] (TTreeReader& r) -> void {
                  const auto thisThreadID = std::this_thread::get_id();
                  unsigned slot;
                  {
                     std::lock_guard<ROOT::TSpinMutex> l(slotMutex);
                     auto thisSlotIt = slotMap.find(thisThreadID);
                     if (thisSlotIt != slotMap.end()) {
                        slot = thisSlotIt->second;
                     } else {
                        slot = globalSlotIndex;
                        slotMap[thisThreadID] = slot;
                        ++globalSlotIndex;
                     }
                  }

                  BuildAllReaderValues(r, slot); // TODO: if TTreeProcessor reuses the same TTreeReader object there is no need to re-build the TTreeReaderValues

                  // recursive call to check filters and conditionally execute actions
                  while(r.Next())
                     for(auto& actionPtr : fBookedActions)
                        actionPtr->Run(slot, r.GetCurrentEntry());
               });
            } else {
#endif // R__USE_IMT
               TTreeReader r;
               if (fTree) r.SetTree(fTree);
               else r.SetTree(fTreeName.c_str(), fDirPtr);

               CreateSlots(1);
               BuildAllReaderValues(r, 0);

               // recursive call to check filters and conditionally execute actions
               while(r.Next())
                  for(auto& actionPtr : fBookedActions)
                     actionPtr->Run(0, r.GetCurrentEntry());
#ifdef R__USE_IMT
            }
#endif // R__USE_IMT

            // forget actions and "detach" the action result pointers marking them ready and forget them too
            fBookedActions.clear();
            for (auto aptr : fActionResultsPtrs) {
               aptr->SetReady();
            }
            fActionResultsPtrs.clear();
         }

         // build reader values for all actions, filters and branches
         void BuildAllReaderValues(TTreeReader& r, unsigned slot) {
            for(auto& ptr : fBookedActions)
               ptr->BuildReaderValues(r, slot);
            for(auto& ptr : fBookedFilters)
               ptr->BuildReaderValues(r, slot);
            for(auto& bookedBranch : fBookedBranches)
               bookedBranch.second->BuildReaderValues(r, slot);
         }

         // inform all actions filters and branches of the required number of slots
         void CreateSlots(unsigned nSlots) {
            for(auto& ptr : fBookedActions)
               ptr->CreateSlots(nSlots);
            for(auto& ptr : fBookedFilters)
               ptr->CreateSlots(nSlots);
            for(auto& bookedBranch : fBookedBranches)
               bookedBranch.second->CreateSlots(nSlots);
         }

         std::weak_ptr<Details::TDataFrameImpl> GetDataFrame() const {
            return fFirstData;
         }

         const BranchList& GetDefaultBranches() const {
            return fDefaultBranches;
         }

         const TDataFrameBranchBase& GetBookedBranch(const std::string& name) const {
            return *fBookedBranches.find(name)->second.get();
         }

         void* GetTmpBranchValue(const std::string& branch, unsigned slot, int entry) {
            return fBookedBranches.at(branch)->GetValue(slot, entry);
         }

         TDirectory* GetDirectory() const {
            return fDirPtr;
         }

         std::string GetTreeName() const {
            return fTreeName;
         }

         void Book(Internal::ActionBasePtr actionPtr) {
            fBookedActions.emplace_back(std::move(actionPtr));
         }

         void Book(Internal::FilterBasePtr filterPtr) {
            fBookedFilters.emplace_back(std::move(filterPtr));
         }

         void Book(TmpBranchBasePtr branchPtr) {
            fBookedBranches[branchPtr->GetName()] = std::move(branchPtr);
         }

         // dummy call, end of recursive chain of calls
         bool CheckFilters(int, unsigned) {
            return true;
         }

         // register a TActionResultPtr
         void RegisterActionResult(TActionResultPtrBase* ptr) {
            fActionResultsPtrs.emplace_back(ptr);
         }

         Internal::ActionBaseVec fBookedActions;
         Internal::FilterBaseVec fBookedFilters;
         std::map<std::string, TmpBranchBasePtr> fBookedBranches;
         std::vector<TActionResultPtrBase*> fActionResultsPtrs;
         std::string fTreeName;
         TDirectory* fDirPtr = nullptr;
         TTree* fTree = nullptr;
         const BranchList fDefaultBranches;
         // always empty: each object in the chain copies this list from the previous
         // and they must copy an empty list from the base TDataFrameImpl
         const BranchList fTmpBranches;
         std::weak_ptr<TDataFrameImpl> fFirstData;
      };

   } // end NS Details

} // end NS ROOT


// Functions and method implementations
namespace ROOT{
   template<typename T>
   TDataFrameInterface<T>::TDataFrameInterface(const std::string& treeName,
                                               TDirectory* dirPtr,
                                               const BranchList& defaultBranches):
      fDerivedPtr(new ROOT::Details::TDataFrameImpl(treeName, dirPtr, defaultBranches)) {
         fDerivedPtr->fFirstData = fDerivedPtr->shared_from_this();
      };

   template<typename T>
   TDataFrameInterface<T>::TDataFrameInterface(TTree& tree, const BranchList& defaultBranches):
      fDerivedPtr(new ROOT::Details::TDataFrameImpl(tree, defaultBranches)) {
         fDerivedPtr->fFirstData = fDerivedPtr->shared_from_this();
      };

   void TActionResultPtrBase::TriggerRun()
   {
      fFirstData.lock()->Run();
   }

   TActionResultPtrBase::TActionResultPtrBase(std::weak_ptr<Details::TDataFrameImpl> firstData) : fFirstData(firstData)
   {
      firstData.lock()->RegisterActionResult(this);
   }

   namespace Details {
      // N.B. these methods could be unified and put in TDataFrameInterface, if I
      // could find a way to make it compile
      template<typename F, typename PrevData>
      template<typename T>
      void TDataFrameFilter<F, PrevData>::Book(std::shared_ptr<T> ptr) {
         fFirstData.lock()->Book(ptr);
      }

      template<typename F, typename PrevData>
      template<typename T>
      void TDataFrameBranch<F, PrevData>::Book(std::shared_ptr<T> ptr) {
         fFirstData.lock()->Book(ptr);
      }
   } //end NS Details

   namespace Internal {
      template<int S, typename T>
      T& GetBranchValue(TVBPtr& readerValue, unsigned slot, int entry, const std::string& branch, std::weak_ptr<Details::TDataFrameImpl> df)
      {
         if(readerValue == nullptr) {
            // temporary branch
            void* tmpBranchVal = df.lock()->GetTmpBranchValue(branch, slot, entry);
            return *static_cast<T*>(tmpBranchVal);
         } else {
            // real branch
            return **std::static_pointer_cast<TTreeReaderValue<T>>(readerValue);
         }
      }
   } //end NS Internal

} // end NS ROOT


// FIXME: need to rethink the print function

#endif // ROOT_TDATAFRAME
