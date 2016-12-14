// TODO
// - build tree of calls
// - extracting filter and foreach from TDataFrame and TDataFrameFilter (they are identical)
#include <list>
#include <vector>
#include <string>
#include <memory>

using BranchList = std::vector<std::string>;


class TDataFrameActionBase {
};


template<typename F, typename PrevDataFrame>
class TDataFrameAction : public TDataFrameActionBase {
   public:
   TDataFrameAction(F f, const BranchList& bl, PrevDataFrame pd)
      : fAction(f), fBranchList(bl), fPrevData(pd) {};
   private:
   F fAction;
   const BranchList& fBranchList;
   PrevDataFrame& fPrevData;
};


template<typename Filter, typename PrevDataFrame>
class TDataFrameFilter {
   template<typename A, typename B> friend class TDataFrameFilter;

   public:
   TDataFrameFilter(Filter f, const BranchList& bl, PrevDataFrame pd)
      : fFilter(f), fBranchList(bl), fPrevData(pd) {}

   template<typename F>
   auto filter(F f, const BranchList& bl) -> TDataFrameFilter<F, decltype(*this)> {
      return TDataFrameFilter<F, decltype(*this)>(f, bl, *this);
   }

   template<typename F>
   void foreach(F f, const BranchList& bl) {
      Book(std::make_shared<TDataFrameAction<F, decltype(*this)>>(f, bl, *this));
   }

   private:
   void Book(std::shared_ptr<TDataFrameActionBase> ptr) {
      fPrevData.Book(ptr);
   }

   Filter fFilter;
   const BranchList& fBranchList;
   PrevDataFrame& fPrevData;
};


class TDataFrame {
   template<typename A, typename B> friend class TDataFrameFilter;

   public:
   template<typename F>
   auto filter(F f, const BranchList& bl) -> TDataFrameFilter<F, decltype(*this)> {
      return TDataFrameFilter<F, decltype(*this)>(f, bl, *this);
   }

   template<typename F>
   void foreach(F f, const BranchList& bl) {
      Book(std::make_shared<TDataFrameAction<F, decltype(*this)>>(f, bl, *this));
   }

   std::vector<std::shared_ptr<TDataFrameActionBase>> fBookedActions;
   private:
   void Book(std::shared_ptr<TDataFrameActionBase> ptr) {
      fBookedActions.push_back(ptr);
   }
};
