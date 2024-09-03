import CategoriesDropdown from './categories-dropdown';
import SearchBar from './search-bar';

export default function SearchControls() {
  return (
    <div>
      <h1 className="text-center text-3xl font-bold text-white">Omi Memories</h1>
      <div className="mb-10 mt-8 flex w-full items-center gap-2">
        <SearchBar />
        <CategoriesDropdown />
      </div>
    </div>
  );
}
